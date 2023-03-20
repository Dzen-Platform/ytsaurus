---
vcsPath: ydb/docs/ru/core/yql/reference/yql-core/syntax/_includes/into_result.md
sourcePath: ydb/docs/ru/core/yql/reference/yql-core/syntax/_includes/into_result.md
---
# INTO RESULT

Позволяет задать пользовательскую метку для [SELECT](../select.md), [PROCESS](../process.md) или [REDUCE](../reduce.md). Не может быть задано одновременно с [DISCARD](../discard.md).

**Примеры:**

``` yql
SELECT 1 INTO RESULT foo;
```

``` yql
SELECT * FROM
my_table
WHERE value % 2 == 0
INTO RESULT `Название результата`;
```