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

/* 跟踪信息结构体 */
typedef struct TraceEntry {
	char *function_name;    /* 函数名称 */
	char *sql_text;        /* SQL文本 */
	TimestampTz exec_time; /* 执行时间 */
	struct TraceEntry *next;
} TraceEntry;

/* 全局变量 */
static TraceEntry *trace_list = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static char *current_function_name = NULL;

/* 辅助函数：获取类型的输入函数 */
static Oid get_type_input_function(Oid type_oid)
{
	HeapTuple tup;
	Oid input_func;
	
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type with OID %u does not exist", type_oid)));
	
	input_func = ((Form_pg_type) tup->t_data)->typinput;
	ReleaseSysCache(tup);
	return input_func;
}

/* 辅助函数：获取函数参数类型 */
static Oid get_func_argtype(Oid func_oid, int argnum)
{
	HeapTuple tup;
	Form_pg_proc proc;
	Oid argtype;
	
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", func_oid)));
	
	proc = (Form_pg_proc) tup->t_data;
	if (argnum >= proc->pronargs)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function %s does not have %d arguments", 
						NameStr(proc->proname), argnum + 1)));
	
	argtype = proc->proargtypes.values[argnum];
	ReleaseSysCache(tup);
	return argtype;
}

/* 钩子函数实现 */
static void trace_executor_start(QueryDesc *queryDesc, int eflags)
{
	TimestampTz start_time = GetCurrentTimestamp();
	TraceEntry *entry;
	MemoryContext old_context;
	
	/* 安全检查 */
	if (!queryDesc || !queryDesc->sourceText)
		return;
	
	/* 切换到新的内存上下文 */
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	
	PG_TRY();
	{
		/* 记录查询开始信息 */
		entry = palloc(sizeof(TraceEntry));
		if (!entry)
		{
			MemoryContextSwitchTo(old_context);
			return;
		}
			
		entry->sql_text = pstrdup(queryDesc->sourceText);
		entry->exec_time = start_time;
		entry->function_name = pstrdup(current_function_name ? current_function_name : "Unknown");
		entry->next = trace_list;
		trace_list = entry;
		
		/* 调用原始钩子 */
		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);
	}
	PG_CATCH();
	{
		/* 确保清理资源 */
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
	
	/* 切换回原来的内存上下文 */
	MemoryContextSwitchTo(old_context);
}

static void trace_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once)
{
	/* 调用原始钩子 */
	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
}

/* 清理跟踪数据 */
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

/* 生成跟踪报告 */
static char* generate_trace_report(void)
{
	StringInfoData str;
	TraceEntry *entry;
	int entry_count = 0;
	
	initStringInfo(&str);
	
	appendStringInfoString(&str, "函数执行跟踪报告\n");
	appendStringInfoString(&str, "==================\n\n");
	
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

/* 解析函数参数 */
static List *parse_function_arguments(const char *func_call_str)
{
	List *args = NIL;
	char *args_str;
	char *token;
	char *saveptr;
	char *end;
	
	/* 提取参数部分 */
	args_str = strchr(func_call_str, '(');
	if (!args_str)
		return NIL;
	
	/* 跳过左括号 */
	args_str++;
	
	/* 找到右括号 */
	end = strrchr(args_str, ')');
	if (!end)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("invalid function call syntax: missing closing parenthesis")));
	
	/* 截取参数部分 */
	*end = '\0';
	
	/* 分割参数 */
	token = strtok_r(args_str, ",", &saveptr);
	while (token != NULL)
	{
		/* 去除空白字符 */
		while (isspace(*token))
			token++;
		
		/* 添加到参数列表 */
		args = lappend(args, makeString(pstrdup(token)));
		token = strtok_r(NULL, ",", &saveptr);
	}
	
	return args;
}

/* 解析函数调用字符串 */
static Oid parse_function_call(const char *func_call_str, List **args)
{
	List *names;
	Oid func_oid;
	Oid *argtypes;
	int nargs;
	char *func_name;
	char *func_name_copy;
	
	/* 安全地复制函数名 */
	func_name_copy = pstrdup(func_call_str);
	func_name = strtok(func_name_copy, "(");
	if (!func_name)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("invalid function call syntax")));
	}
	
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
	
	/* 获取函数OID */
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

/* 动态执行函数 */
static Datum execute_function(Oid func_oid, const char *func_call_str, List *args)
{
	FunctionCallInfoData fcinfo;
	FmgrInfo flinfo;
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
				 errmsg("invalid function call parameters")));
	
	/* 初始化函数调用信息 */
	fmgr_info(func_oid, &flinfo);
	InitFunctionCallInfoData(fcinfo, &flinfo, flinfo.fn_nargs, 
						   InvalidOid, NULL, NULL);
	
	/* 设置函数名称用于跟踪 */
	func_name_copy = pstrdup(func_call_str);
	func_name = strtok(func_name_copy, "(");
	if (!func_name)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("invalid function call syntax")));
	}
	
	name_list = stringToQualifiedNameList(func_name);
	if (name_list == NIL)
	{
		pfree(func_name_copy);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("invalid function name")));
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
		
		/* 参数检查 */
		if (!arg_str)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("invalid argument value")));
		
		/* 获取参数类型 */
		arg_type = get_func_argtype(func_oid, i);
		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("invalid argument type for function")));
		
		/* 获取输入函数 */
		input_func = get_type_input_function(arg_type);
		if (input_func == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("no input function available for type %s", 
							format_type_be(arg_type))));
		
		/* 转换参数值 */
		PG_TRY();
		{
			fmgr_info(input_func, &input_flinfo);
			arg_value = InputFunctionCall(&input_flinfo, arg_str, arg_type, -1);
		}
		PG_CATCH();
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: %s", 
							format_type_be(arg_type), arg_str)));
		}
		PG_END_TRY();
		
		fcinfo.arg[i] = arg_value;
		fcinfo.argnull[i] = false;
		i++;
	}
	
	/* 检查参数数量 */
	if (i != flinfo.fn_nargs)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("wrong number of arguments: got %d, expected %d", 
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
				 errmsg("function returned NULL")));
	
	return result;
}

/* 函数声明 */
PG_FUNCTION_INFO_V1(pg_trace_tool);

/* 函数定义 */
Datum pg_trace_tool(PG_FUNCTION_ARGS)
{
	text *funcname = PG_GETARG_TEXT_PP(0);
	char *result;
	Oid func_oid;
	List *args = NIL;
	char *func_call_str;
	MemoryContext old_context;
	
	/* 参数检查 */
	if (!funcname)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("function name cannot be NULL")));
	}
	
	/* 获取函数调用字符串 */
	func_call_str = text_to_cstring(funcname);
	if (!func_call_str)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("invalid function call string")));
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
		
		ExecutorStart_hook = prev_ExecutorStart;
		ExecutorRun_hook = prev_ExecutorRun;
		
		cleanup_trace_data();
		
		if (current_function_name)
		{
			pfree(current_function_name);
			current_function_name = NULL;
		}
		
		MemoryContextSwitchTo(old_context);
		
		PG_RETURN_CSTRING(result);
	}
	PG_CATCH();
	{
		/* 确保清理所有资源 */
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
