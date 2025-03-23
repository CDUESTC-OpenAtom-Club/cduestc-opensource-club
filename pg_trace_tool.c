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
#include "funcapi.h"
#include "fmgr.h"
#include <stdio.h>
#include <string.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_trace_tool);

Datum pg_trace_tool(PG_FUNCTION_ARGS)
{
	PG_RETURN_CSTRING("Hello, World!");
}
