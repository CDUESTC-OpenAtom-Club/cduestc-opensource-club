/*
 * pg_trace_tool.c
 *
 * A trace tool for OpenTenBase that can be used to track and analyze the execution of Functions and Procedures.
 * Supported features:
 * 1. Track and analyze Function and Procedure execution
 * 2. Generate execution reports for Functions and Procedures
 *
 * Author: CDUESTC Open Atom Open Source Club
 * Version: 1.0	
 */

#include "c.h"
#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"
#include "executor/execdesc.h"
#include "utils/timestamp.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "catalog/pg_proc.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "access/htup.h"
#include "parser/parse_func.h"
#include "utils/elog.h"
#include "pg_trace_tool.h"

PG_MODULE_MAGIC;


/* 跟踪信息结构体 单向链表 */
typedef struct TraceEntry {
	/* 函数名称 */
	char *function_name;
	/* SQL文本 */
	char *sql_text;
	/* 执行时间 */
	TimestampTz exec_time;
	/* 下一个结构体 */
	struct TraceEntry *next;
} TraceEntry;


/* 全局变量 */
static TraceEntry *trace_list = NULL;
/* 原有的钩子函数 */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
/* 当前正在执行的function的名称 */
static char *current_function_name = NULL;


/**
	辅助函数：获取指定类型的输入函数。
	@param type_oid 要查询的类型的oid。如text、int32等的oid。
	@return Oid 对应类型的输入函数的oid
 */
static Oid get_type_input_function(Oid type_oid)
{
	/* 存储从系统缓存里查询到的元组 */
	HeapTuple tup;
	/* 保存结果，即输入函数的OID */
	Oid input_func;
	
	/* 
		根据oid，从系统的pg_type表里查找对应的输入函数类型
		ObjectIdGetDatum函数用于将OID转换为Datum类型用于查询
	*/
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("The type with Oid %u does not exist", type_oid)));
	input_func = ((Form_pg_type) tup->t_data)->typinput;

	ReleaseSysCache(tup);
	return input_func;
}

/**
	辅助函数：获取函数参数类型。
	@param func_oid 要查询参数类型的function的OID
	@param argnum 要获取的参数的索引
	@return Oid 指定函数中指定参数的类型
 */
static Oid get_func_argtype(Oid func_oid, int argnum)
{
	HeapTuple tup;
	/* 
		pg_proc表存放数据库的function和procedure等信息
		proc变量用于存储指定function/procedure的元数据
	*/
	Form_pg_proc proc;
	/* 保存结果。即指定function中某参数的类型oid */
	Oid argtype;

	/* 根据oid，从pg_proc里查询指定function的元数据 */
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("The function with Oid %u does not exist", func_oid)));
	/* 将查询到的指定function的元数据存储在proc中 */
	proc = (Form_pg_proc) tup->t_data;

	/* 
		proc->pronargs属性为参数个数
		当给定的参数索引大于参数个数时，抛出错误
	*/
	if (argnum >= proc->pronargs)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("The function %s does not have %d arguments", 
						NameStr(proc->proname), argnum + 1)));

	argtype = proc->proargtypes.values[argnum];
	ReleaseSysCache(tup);

	return argtype;
}

/**
	钩子函数。在查询执行器开始运行时进行跟踪。
	@param queryDesc 这个结构体里包含原始SQL文本与执行查询所需的所有上下文信息
	@param eflags 控制查询行为的标志
	@return 无
 */
static void trace_executor_start(QueryDesc *queryDesc, int eflags)
{
	/* 获取查询开始执行的时间 */
	TimestampTz start_time = GetCurrentTimestamp();
	/* 用于跟踪function/procedure执行过程的结构体 */
	TraceEntry *entry;
	/* 用于保存现有的内存上下文 */
	MemoryContext old_context;
	
	if (!queryDesc || !queryDesc->sourceText)
		return;
	
	/* 切换到全局内存上下文，否则无法长期保存跟踪信息 */
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	
	PG_TRY();
	{
		/* 记录查询开始信息 */
		entry = palloc(sizeof(TraceEntry));
		/* 内存分配失败 */
		if (!entry)
		{
			MemoryContextSwitchTo(old_context);
			ereport(ERROR,
            (errcode(ERRCODE_OUT_OF_MEMORY),
             errmsg("Memory allocation failed: unable to allocate memory for query tracking entry"),
             errdetail("Current memory context: %s", old_context->name)));
		}
		
		/* 获取原始SQL文本、开始时间、function名称 */
		entry->sql_text = pstrdup(queryDesc->sourceText);
		entry->exec_time = start_time;
		entry->function_name = pstrdup(current_function_name ? current_function_name : "Unknown");
		entry->next = trace_list;
		trace_list = entry;
		
		/* 调用其他钩子 */
		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
	}
	PG_CATCH();
	{
		/* 出错后清理资源 */
		if (entry)
		{
			if (entry->sql_text)
				pfree(entry->sql_text);
			if (entry->function_name)
				pfree(entry->function_name);
			pfree(entry);
		}
		
		/* 切换回原来的内存上下文 */
		MemoryContextSwitchTo(old_context);
		
		PG_RE_THROW();
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old_context);
}

/**
	钩子函数。在查询执行器的运行阶段插入，确保数据库的查询执行链不断裂。
 */
static void trace_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
{
	/* 调用原始钩子 */
	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
}

/**
	清理跟踪数据，释放跟踪信息结构体的内存。
 */
static void cleanup_trace_data(void)
{
	TraceEntry *entry = trace_list;
	while (entry != NULL)
	{
		TraceEntry *next = entry->next;
		pfree(entry->sql_text);
		pfree(entry->function_name);
		pfree(entry);
		entry = next;
	}
	trace_list = NULL;
}

/**
	生成跟踪报告。
 */
static char* generate_trace_report(void)
{
	/* 报告结果字符串 */
	StringInfoData str;
	/* 跟踪信息结构体链表 */
	TraceEntry *entry;
	/* 记录执行节点个数 */
	int entry_count = 0;
	
	initStringInfo(&str);
	appendStringInfoString(&str, "函数执行跟踪报告\n");
	appendStringInfoString(&str, "==================\n\n");
	
	/* 便利跟踪信息结构体链表，将跟踪得到的数据写入结果字符串中 */
	entry = trace_list;
	while (entry != NULL)
	{
		entry_count++;
		appendStringInfo(&str, "执行记录 #%d:\n", entry_count);
		appendStringInfo(&str, "----------------\n");
		appendStringInfo(&str, "函数名称: %s\n", entry->function_name);
		appendStringInfo(&str, "SQL语句: %s\n", entry->sql_text);
		appendStringInfo(&str, "执行时间: %s\n", 
						timestamptz_to_str(entry->exec_time));
		appendStringInfoString(&str, "\n");
		entry = entry->next;
	}
	
	if (entry_count == 0)
	{
		appendStringInfoString(&str, "没有找到任何执行记录\n");
	}
	else
	{
		appendStringInfo(&str, "总计执行记录数: %d\n", entry_count);
	}
	
	return str.data;
}

/**
	解析函数参数。
	@param func_call_str 指定function的调用语句字符串
	@return List 指定function的参数列表指针
 */
static List *parse_function_arguments(const char *func_call_str)
{
	/* 保存结果。即指定function的参数列表 */
	List *args = NIL;
	/* 保存function调用语句字符串中的参数部分 */
	char *args_str;
	/* 临时存储当前正在解析的参数字符串，为NULL时代表解析完毕 */
	char *tmpToken;
	/* 在分割参数字符串时，记录分割的进度 */
	char *saveptr;
	/* function的调用字符串中参数部分的结束指针 */
	char *end;
	
	/* 以左括号为起点开始提取参数部分，并跳过左括号 */
	args_str = strchr(func_call_str, '(');
	if (!args_str)
		return NIL;
	args_str++;
	
	/* 找到右括号 */
	end = strrchr(args_str, ')');
	if (!end)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Invalid function call syntax: missing closing parenthesis")));
	/* 截取字符串 */
	*end = '\0';

	/* 
		至此args_str中存储的是function调用字符串中的参数部分。
		假设有function：my_function(1, 'hello')
		则此时args_str为 {'1', ',', ' ', 'h', 'e', 'l', 'l', 'o'}。
	 */
	
	/* 按逗号分割参数 */
	tmpToken = strtok_r(args_str, ",", &saveptr);
	while (tmpToken != NULL)
	{
		/* 去除空白字符 */
		while (isspace(*tmpToken))
			tmpToken++;
		/* 将参数字符串添加到List */
		args = lappend(args, makeString(pstrdup(tmpToken)));
		tmpToken = strtok_r(NULL, ",", &saveptr);
	}
	
	return args;
}

/**
	解析函数调用字符串，返回该函数的Oid。同时解析该function的参数列表。
	@param func_call_str 指定function的调用字符串
	@param args 指定function的参数列表的指针
	@return Oid 指定function的Oid
 */
static Oid parse_function_call(const char *func_call_str, List **args)
{
	/* 存储函数名称，使用链表是因为函数调用时有可能使用schema.func的形式 */
	List *names;
	/* 存储结果。即指定function的Oid */
	Oid func_oid;
	/* 存储指定function的参数类型列表 */
	Oid *argtypes;
	/* 记录指定function的参数个数 */
	int nargs;
	char *func_name;
	char *func_name_copy;
	
	func_name_copy = pstrdup(func_call_str);
	/* 第一个小括号前的部分即为function字符串 */
	func_name = strtok(func_name_copy, "(");
	if (!func_name)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Invalid function call syntax")));
	}
	/* 将得到的字符串转换为函数名称链表 */
	names = stringToQualifiedNameList(func_name);
	if (names == NIL)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("invalid function call syntax")));
	}
	
	/* 解析参数 */
	*args = parse_function_arguments(func_call_str);
	nargs = list_length(*args);
	
	/* 根据函数名称、参数数量与参数类型列表查找函数Oid */
	argtypes = palloc0(nargs * sizeof(Oid));
	func_oid = LookupFuncName(names, nargs, argtypes, false);
	pfree(argtypes);
	
	if (func_oid == InvalidOid)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist", NameListToString(names))));
	}
	
	pfree(func_name_copy);
	return func_oid;
}

/**
	动态执行函数
	@param func_oid 指定function的Oid
	@param func_call_str 指定function的调用语句
	@param args 指定function的参数列表
	@return Datum 运行指定的function后产生的返回值
 */
static Datum execute_function(Oid func_oid, const char *func_call_str, List *args)
{
	FunctionCallInfoData fcinfo;
	/* 存储指定function的元数据，包括函数的调用入口、参数等信息 */
	FmgrInfo flinfo;
	/* 存储结果。即指定function运行后的返回值 */
	Datum result;
	ListCell *lc;
	int i;
	char *func_name;
	char *func_name_copy;
	List *name_list = NIL;
	
	/* 参数检查 */
	if (!func_call_str || !args)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Invalid function call parameters")));
	
	/* 初始化函数调用信息和函数调用上下文 */
	fmgr_info(func_oid, &flinfo);
	InitFunctionCallInfoData(fcinfo, &flinfo, flinfo.fn_nargs, 
						   InvalidOid, NULL, NULL);
	
	/* 解析并设置函数名称用于跟踪 */
	func_name_copy = pstrdup(func_call_str);
	func_name = strtok(func_name_copy, "(");
	if (!func_name)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Invalid function call syntax")));
	}
	
	name_list = stringToQualifiedNameList(func_name);
	if (name_list == NIL)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Invalid function name")));
	}
	
	if (current_function_name)
		pfree(current_function_name);
	current_function_name = pstrdup(NameListToString(name_list));
	pfree(func_name_copy);
	
	/* 设置参数 */
	i = 0;
	foreach(lc, args)
	{
		char *arg_str = (char *) lfirst(lc);
		Datum arg_value;
		Oid arg_type;
		Oid input_func;
		FmgrInfo input_flinfo;
		
		if (!arg_str)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("Invalid argument value")));
		
		/* 获取参数类型 */
		arg_type = get_func_argtype(func_oid, i);
		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("Invalid argument type for function")));
		
		/* 获取输入函数 */
		input_func = get_type_input_function(arg_type);
		if (input_func == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("No input function available for type %s", 
							format_type_be(arg_type))));
		
		/* 转换参数值 */
		PG_TRY();
		{
			fmgr_info(input_func, &input_flinfo);
			arg_value = InputFunctionCall(&input_flinfo, arg_str, arg_type, -1);
		}
		PG_CATCH();
		{
			/* 参数转换失败 */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("Invalid input syntax for type %s: %s", 
							format_type_be(arg_type), arg_str)));
		}
		PG_END_TRY();
		
		/* 填充参数 */
		fcinfo.arg[i] = arg_value;
		fcinfo.argnull[i] = false;
		i++;
	}
	
	/* 检查参数数量 */
	if (i != flinfo.fn_nargs)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Wrong number of arguments: got %d, expected %d", 
						i, flinfo.fn_nargs)));
	
	/* 执行函数 */
	PG_TRY();
	{
		result = FunctionCallInvoke(&fcinfo);
	}
	PG_CATCH();
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("function execution failed")));
	}
	PG_END_TRY();
	
	/* 检查执行结果 */
	if (fcinfo.isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("Function call failed")));
	
	return result;
}

/* 函数声明 */
PG_FUNCTION_INFO_V1(pg_trace_tool);

/* 函数定义 */
Datum pg_trace_tool(PG_FUNCTION_ARGS)
{
	/* 获取function调用字符串 */
	text *funcname = PG_GETARG_TEXT_PP(0);
	/* 追踪结果 */
	char *result;
	/* 解析出的function的Oid */
	Oid func_oid;
	/* 解析出的function的参数列表 */
	List *args = NIL;
	/* function的调用字符串 */
	char *func_call_str;
	/* 用于存储当前内存上下文呢 */
	MemoryContext old_context;
	
	/* 参数检查 */
	if (!funcname)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Function name cannot be NULL")));
	}
	
	/* 获取函数调用字符串 */
	func_call_str = text_to_cstring(funcname);
	if (!func_call_str)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("Invalid function call string")));
	}
	
	/* 切换到新的内存上下文 */
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	
	PG_TRY();
	{
		/* 清理之前的跟踪数据 */
		cleanup_trace_data();
		
		/* 保存原始钩子 */
		prev_ExecutorStart = ExecutorStart_hook;
		prev_ExecutorRun = ExecutorRun_hook;
		
		/* 设置新的钩子 */
		ExecutorStart_hook = trace_executor_start;
		ExecutorRun_hook = trace_executor_run;
		
		/* 解析并执行函数 */
		func_oid = parse_function_call(func_call_str, &args);
		execute_function(func_oid, func_call_str, args);
		
		list_free_deep(args);
		result = generate_trace_report();
		
		/* 执行完成，还原原始钩子并清理跟踪数据 */
		ExecutorStart_hook = prev_ExecutorStart;
		ExecutorRun_hook = prev_ExecutorRun;
		cleanup_trace_data();
		if (current_function_name)
		{
			pfree(current_function_name);
			current_function_name = NULL;
		}
		
		/* 切换回原来的内存上下文 */
		MemoryContextSwitchTo(old_context);
		
		/* 返回跟踪数据，该数据会在数据库控制台打印出来 */
		PG_RETURN_CSTRING(result);
	}
	PG_CATCH();
	{
		/* 发生错误，清理所有资源并退出 */
		if (args != NIL)
			list_free_deep(args);
		cleanup_trace_data();
		if (current_function_name)
		{
			pfree(current_function_name);
			current_function_name = NULL;
		}
		
		/* 恢复原始钩子 */
		ExecutorStart_hook = prev_ExecutorStart;
		ExecutorRun_hook = prev_ExecutorRun;
		
		/* 切换回原来的内存上下文 */
		MemoryContextSwitchTo(old_context);
		
		PG_RE_THROW();
	}
	PG_END_TRY();
}
