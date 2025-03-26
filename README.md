# OpenTenBase核心贡献挑战赛 - 赛题四

# 结果

完成度：

- [x] 每一步调用的指令/SQL文本

- [ ] 每一步调用所在的方法行数

- [x] 每一步调用所在的方法名称

- [ ] 每一步调用时的变量信息

- [x] 每一步触发的时间点

测试用function

![测试用function](./result/测试函数.png)

测试结果

![测试结果](./result/测试结果.png)

# 使用文档

pg_trace_tool 是一个用于 OpenTenBase 的函数执行跟踪工具，可以帮助用户跟踪和分析函数与存储过程的执行过程。

## 功能特点

- 跟踪函数和存储过程的执行过程
- 记录执行的 SQL 语句
- 记录执行时间
- 生成详细的执行报告

## 安装说明

1. 编译插件：
```bash
cd /path/to/OpenTenBase/contrib/
git clone https://atomgit.com/da_capo_/pg_trace_tool.git
make
make install
```

2. 在数据库中创建扩展：
```sql
CREATE EXTENSION pg_trace_tool;
```

## 使用方法

### 基本用法

1. 创建测试函数：
```sql
CREATE OR REPLACE FUNCTION test_simple_func(a int, b int)
RETURNS int
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN a + b;
END;
$$;
```

2. 使用 pg_trace_tool 跟踪函数执行：
```sql
SELECT pg_trace_tool('test_simple_func(1, 2)');
```

### 输出格式

跟踪报告将包含以下信息：
```
函数执行跟踪报告
==================

执行记录 #1:
----------------
函数名称: test_simple_func
SQL语句: SELECT test_simple_func(1, 2)
执行时间: 2024-03-21 10:30:45.123456+08:00

执行记录 #2:
----------------
函数名称: test_simple_func
SQL语句: RETURN a + b
执行时间: 2024-03-21 10:30:45.123457+08:00

总计执行记录数: 2
```

## 注意事项

1. 插件会记录所有 SQL 语句的执行，包括函数调用和函数内部的 SQL 语句
2. 每次跟踪都会生成新的报告，不会保留历史记录
3. 确保在跟踪前清理之前的跟踪数据

## 示例

### 简单函数示例

```sql
-- 创建简单函数
CREATE OR REPLACE FUNCTION test_simple_func(a int, b int)
RETURNS int
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN a + b;
END;
$$;

-- 跟踪函数执行
SELECT pg_trace_tool('test_simple_func(1, 2)');
```

### 复杂函数示例

```sql
-- 创建复杂函数
CREATE OR REPLACE FUNCTION test_complex_func()
RETURNS void
LANGUAGE plpgsql
AS $$
BEGIN
    -- 创建临时表
    CREATE TEMP TABLE temp_test (id int, value text);
    
    -- 插入数据
    INSERT INTO temp_test VALUES (1, 'test1');
    INSERT INTO temp_test VALUES (2, 'test2');
    
    -- 查询数据
    SELECT * FROM temp_test;
    
    -- 删除临时表
    DROP TABLE temp_test;
END;
$$;

-- 跟踪函数执行
SELECT pg_trace_tool('test_complex_func()');
```

## 错误处理

如果遇到错误，插件会提供详细的错误信息，包括：
- 函数不存在
- 参数类型不匹配
- 语法错误
- 执行错误

## 限制

1. 目前仅支持跟踪函数和存储过程的执行
2. 不支持跟踪触发器
3. 不支持跟踪事件触发器
4. 不支持跟踪规则
