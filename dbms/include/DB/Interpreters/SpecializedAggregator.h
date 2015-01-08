#include <DB/Interpreters/Aggregator.h>

#include <DB/AggregateFunctions/AggregateFunctionCount.h>
#include <DB/AggregateFunctions/AggregateFunctionSum.h>
#include <DB/AggregateFunctions/AggregateFunctionAvg.h>
#include <DB/AggregateFunctions/AggregateFunctionsMinMaxAny.h>
#include <DB/AggregateFunctions/AggregateFunctionsArgMinMax.h>
#include <DB/AggregateFunctions/AggregateFunctionUniq.h>
#include <DB/AggregateFunctions/AggregateFunctionUniqUpTo.h>
#include <DB/AggregateFunctions/AggregateFunctionGroupArray.h>
#include <DB/AggregateFunctions/AggregateFunctionGroupUniqArray.h>
#include <DB/AggregateFunctions/AggregateFunctionQuantile.h>
#include <DB/AggregateFunctions/AggregateFunctionQuantileTiming.h>
#include <DB/AggregateFunctions/AggregateFunctionIf.h>
#include <DB/AggregateFunctions/AggregateFunctionArray.h>
#include <DB/AggregateFunctions/AggregateFunctionState.h>
#include <DB/AggregateFunctions/AggregateFunctionMerge.h>


namespace DB
{


/** Шаблон цикла агрегации, позволяющий сгенерировать специализированный вариант для конкретной комбинации агрегатных функций.
  * Отличается от обычного тем, что вызовы агрегатных функций должны инлайниться, а цикл обновления агрегатных функций должен развернуться.
  *
  * Так как возможных комбинаций слишком много, то не представляется возможным сгенерировать их все заранее.
  * Этот шаблон предназначен для того, чтобы инстанцировать его в рантайме,
  *  путём запуска компилятора, компиляции shared library и использования её с помощью dlopen.
  */


/** Список типов - для удобного перечисления агрегатных функций.
  */
template <typename THead, typename... TTail>
struct TypeList
{
	using Head = THead;
	using Tail = TypeList<TTail...>;

	static constexpr size_t size = 1 + sizeof...(TTail);

	template <size_t I>
	using At = typename std::template conditional<I == 0, Head, typename Tail::template At<I - 1>>::type;

	template <typename Func, size_t index = 0>
	static void forEach(Func && func)
	{
		func.template operator()<Head, index>();
		Tail::template forEach<Func, index + 1>(std::forward<Func>(func));
	}
};


template <typename THead>
struct TypeList<THead>
{
	using Head = THead;

	static constexpr size_t size = 1;

	template <size_t I>
	using At = typename std::template conditional<I == 0, Head, std::nullptr_t>::type;

	template <typename Func, size_t index = 0>
	static void forEach(Func && func)
	{
		func.template operator()<Head, index>();
	}
};


struct EmptyTypeList
{
	static constexpr size_t size = 0;

	template <size_t I>
	using At = std::nullptr_t;

	template <typename Func, size_t index = 0>
	static void forEach(Func && func)
	{
	}
};


struct AggregateFunctionsUpdater
{
	AggregateFunctionsUpdater(
		const Aggregator::AggregateFunctionsPlainPtrs & aggregate_functions_,
		const Sizes & offsets_of_aggregate_states_,
		Aggregator::AggregateColumns & aggregate_columns_,
		AggregateDataPtr & value_,
		size_t row_num_)
		: aggregate_functions(aggregate_functions_),
		offsets_of_aggregate_states(offsets_of_aggregate_states_),
		aggregate_columns(aggregate_columns_),
		value(value_), row_num(row_num_)
	{
	}

	template <typename AggregateFunction, size_t column_num>
	void operator()() ALWAYS_INLINE;

	const Aggregator::AggregateFunctionsPlainPtrs & aggregate_functions;
	const Sizes & offsets_of_aggregate_states;
	Aggregator::AggregateColumns & aggregate_columns;
	AggregateDataPtr & value;
	size_t row_num;
};

template <typename AggregateFunction, size_t column_num>
void AggregateFunctionsUpdater::operator()()
{
	reinterpret_cast<AggregateFunction *>(aggregate_functions[column_num])->add(
		value + offsets_of_aggregate_states[column_num],
		&aggregate_columns[column_num][0],
		row_num);
}

struct AggregateFunctionsCreator
{
	AggregateFunctionsCreator(
		const Aggregator::AggregateFunctionsPlainPtrs & aggregate_functions_,
		const Sizes & offsets_of_aggregate_states_,
		Aggregator::AggregateColumns & aggregate_columns_,
		AggregateDataPtr & aggregate_data_)
		: aggregate_functions(aggregate_functions_),
		offsets_of_aggregate_states(offsets_of_aggregate_states_),
		aggregate_data(aggregate_data_)
	{
	}

	template <typename AggregateFunction, size_t column_num>
	void operator()()
	{
		AggregateFunction * func = static_cast<AggregateFunction *>(aggregate_functions[column_num]);

		try
		{
			/** Может возникнуть исключение при нехватке памяти.
			  * Для того, чтобы потом всё правильно уничтожилось, "откатываем" часть созданных состояний.
			  * Код не очень удобный.
			  */
			func->create(aggregate_data + offsets_of_aggregate_states[column_num]);
		}
		catch (...)
		{
			for (size_t rollback_j = 0; rollback_j < column_num; ++rollback_j)
				func->destroy(aggregate_data + offsets_of_aggregate_states[rollback_j]);

			aggregate_data = nullptr;
			throw;
		}
	}

	const Aggregator::AggregateFunctionsPlainPtrs & aggregate_functions;
	const Sizes & offsets_of_aggregate_states;
	AggregateDataPtr & aggregate_data;
};


template <typename Method, typename AggregateFunctionsList>		/// TODO Обновить.
void Aggregator::executeSpecialized(
	Method & method,
	Arena * aggregates_pool,
	size_t rows,
	ConstColumnPlainPtrs & key_columns,
	AggregateColumns & aggregate_columns,
	const Sizes & key_sizes,
	StringRefs & keys,
	bool no_more_keys,
	AggregateDataPtr overflow_row) const
{
	method.init(key_columns);

	/// Для всех строчек.
	for (size_t i = 0; i < rows; ++i)
	{
		typename Method::iterator it;
		bool inserted;			/// Вставили новый ключ, или такой ключ уже был?
		bool overflow = false;	/// Новый ключ не поместился в хэш-таблицу из-за no_more_keys.

		/// Получаем ключ для вставки в хэш-таблицу.
		typename Method::Key key = method.getKey(key_columns, keys_size, i, key_sizes, keys);

		if (Method::never_overflows || !no_more_keys)	/// Вставляем.
			method.data.emplace(key, it, inserted);
		else
		{
			/// Будем добавлять только если ключ уже есть.
			inserted = false;
			it = method.data.find(key);
			if (method.data.end() == it)
				overflow = true;
		}

		/// Если ключ не поместился, и данные не надо агрегировать в отдельную строку, то делать нечего.
		if (!Method::never_overflows && overflow && !overflow_row)
			continue;

		/// Если вставили новый ключ - инициализируем состояния агрегатных функций, и возможно, что-нибудь связанное с ключом.
		if (inserted)
		{
			method.onNewKey(it, keys_size, i, keys, *aggregates_pool);

			AggregateDataPtr & aggregate_data = Method::getAggregateData(it->second);
			aggregate_data = aggregates_pool->alloc(total_size_of_aggregate_states);

			AggregateFunctionsList::forEach(AggregateFunctionsCreator(
				aggregate_functions, offsets_of_aggregate_states, aggregate_columns, aggregate_data));
		}

		AggregateDataPtr value = (Method::never_overflows || !overflow) ? Method::getAggregateData(it->second) : overflow_row;

		/// Добавляем значения в агрегатные функции.
		AggregateFunctionsList::forEach(AggregateFunctionsUpdater(
			aggregate_functions, offsets_of_aggregate_states, aggregate_columns, value, i));
	}
}

}
