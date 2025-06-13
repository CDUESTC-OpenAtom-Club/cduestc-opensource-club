/*
 * pg_trace_tool.sql
 *
 * This extension provides tools for tracing OpenTenBase execution.
 * It allows tracing and analyzing Function and Procedure execution.
 * 
 * Author: CDUESTC Open Atom Open Source Club
 * Version: 1.0
 */  

-- 创建函数
CREATE OR REPLACE FUNCTION pg_trace_tool(text)
RETURNS CSTRING
AS 'pg_trace_tool', 'pg_trace_tool'
LANGUAGE C;