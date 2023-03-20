# Instance configuration

The article contains information about the configuration of CHYT instances.

The CHYT configuration is a [YSON document](../../../../../user-guide/storage/yson-docs.md) consisting of two sections: the [`clickhouse`](#ch-conf) section and the [`yt`](#yt) section. The first section contains settings that affect the logic of the ClickHouse computation engine: they are almost identical to the original ClickHouse engine settings. The second section contains the settings of the {{product-name}} Runtime built into the CHYT instances: these settings strongly resemble many of the settings from, for example, the operation specification.

An important difference between CHYT and original ClickHouse is that the settings are specified not in XML format, but in YSON format that is native for {{product-name}}. The principle by which the settings are interpreted in this format is described below.

## How the configuration is formed { #how-form }

The configuration is formed sequentially by overlaying the following documents:

- The basic config from Cypress. By default, this is a document from `//sys/clickhouse/config`. You can specify a different document using the `cypress-base-config-path` option when you start the clique.
- The computed part of the config generated by the startup procedure. Contains various memory and CPU limits, as well as other metainformation.
- The config passed in the `clickhouse-config` option when you start the clique.
- The native cluster connection configuration taken from the contents of `//sys/@cluster_connection`.

{% note warning "Attention!" %}

All clique instances always work with the same configuration and it is impossible to change the configuration without restarting the clique.

{% endnote %}

## {{product-name}} part configuration { #yt }

The {{product-name}} part of the configuration is in the `yt` top-level section.

- `settings`: CHYT-specific default query [settings](../../../../../user-guide/data-processing/chyt/reference/settings.md). Changing the setting in this section changes the default value for this setting for all queries in the clique.

- `table_writer`: [Table Writer configuration](../../../../../user-guide/storage/io-configuration.md#table_writer).

- `table_attribute_cache`: Table attribute cache configuration. This cache significantly improves the responsiveness of CHYT, but at the moment it can potentially lead to non-consistent reads (data has already appeared in the table, but CHYT does not see it yet). To disable this cache, use the following configuration:
   ```
   {read_from=follower;expire_after_successful_update_time=0;expire_after_failed_update_time=0;refresh_time=0;expire_after_access_time=0}
   ```
- `create_table_default_attributes` [`{optimize_for = scan}`]: The default attributes with which tables will be created during `CREATE` queries in CHYT.

- `health_checker`: The Health Checker configuration, see also the [corresponding section](../../../../../user-guide/data-processing/chyt/cliques/administration.md#health_checker) on the CHYT dashboard. Consists of 3 fields:

   — `queries` [``["select * from `//sys/clickhouse/sample_table`"]``]: A list of test queries whose performance will be checked regularly.

   {% note warning "Attention!" %}

   Queries from Health Checker are currently executed on behalf of user `yt-clickhouse` who must have the `read` permissions to access the table.

   If you need to start queries on other tables, contact the support chat (link TODO).

   {% endnote %}

   — `period` [60000]:The test triggering period in milliseconds.

   — `timeout` [`0.9 * period / len(queries)`]: The triggering timeout for each of the configured queries. If the query does not fit into the timeout, it is considered `failed`.

- `subquery`: The configuration of the main system part which coordinates the execution of ClickHouse queries on top of {{product-name}} tables. Before configuring this part, read the article about [query execution within a clique](../../../../../user-guide/data-processing/chyt/queries/anatomy.md#query-execution).

   — `min_data_weight_per_thread` [64 MiB]: (in bytes) When splitting the input into subqueries, the coordinator will try to give no less than the given amount to each core of each instance.

   — `max_data_weight_per_subquery` [50 GiB]: (in bytes) The maximum allowable amount of data to be processed on one core of one instance. This restriction is protective and protects clique users from accidentally running a huge query that processes petabytes. The constant of 50 GiB is selected, because such amount is processed on a single core in about dozens of minutes.

- `show_tables`: The `SHOW TABLES` query behavior configuration. Enables you to configure the list of directories in which `SHOW TABLES` will show a list of tables.

   — `roots`: The list of YPath paths of directories in Cypress from which the tables will be collected for `SHOW TABLES`.

{% note warning "Attention!" %}

The `max_data_weight_per_subquery` limit uses column-by-column statistics to account for column sampling from processed tables. Column-by-column statistics may be missing for old tables created before the statistics became available. For such tables, columns in the limit are not taken into account. This means that when processing a very narrow slice of columns which forms 1% of the total table volume and is 1 GiB per core, CHYT will calculate that 100 GiB per core are processed. CHYT will not run such a query with the default settings.

If this scenario is required or you need to process large amounts of data, configure this setting to a random large value.

{% endnote %}

## Configuring a ClickHouse part { #ch-conf }

The ClickHouse part of the configuration lives in the `clickhouse` top-level section.

### Converting a ClickHouse XML configuration into a CHYT YSON configuration { #convert_xml_to_yson }

This section describes how to convert a regular `clickhouse server` XML configuration into an equivalent CHYT configuration.

The rules for converting a ClickHouse XML configuration into a CHYT YSON configuration can be described as follows:

- Any non-multiple configuration node in the ClickHouse configuration is a dict (`map`).

- A multiple node is represented by a list (`list`).

Below is an example of converting an artificial XML configuration into a YSON configuration.


```xml
<foo>42</foo>
<bar>qwe</bar>
<baz>
    <quux>3.14</quux>
</baz>
<baz></baz>
<baz>hi!</baz>
```


```json
{
    foo = 42;
    bar = "qwe";
    baz = [
        {quux = 3.14};
        {};
        "hi!";
    ];
}
```

### The `clickhouse` section { #clickhouse }

The `clickhouse` section is passed to the ClickHouse configuration. Below are two parts of this section that may be convenient in CHYT:

- `settings`: Contains the [query settings](https://clickhouse.com/docs/en/operations/settings/settings/) that will be applied to a default user profile, i.e. the profile from which the settings for all queries will be taken. For example, you can write the `{extremes = 1}` dict to this section to enable the [extremes](https://clickhouse.yandex/docs/ru/operations/settings/settings/#extremes) ClickHouse option.

- `dictionaries` (`[]`): The configuration of external dicts. The value must be a list of dict configurations. Each dict is configured by `map` with the following fields that retain the meaning of the original ClickHouse configuration:
   - `name`: The name of the external dict.
   - `source`: The [data source](https://clickhouse.com/docs/en/sql-reference/dictionaries/external-dictionaries/external-dicts-dict-sources/) for the external dict.
   - `layout`: [Representation](https://clickhouse.com/docs/en/sql-reference/dictionaries/external-dictionaries/external-dicts-dict-layout/) of the external dict in the instance memory.
   - `structure`: The [schema of data](https://clickhouse.com/docs/en/sql-reference/dictionaries/external-dictionaries/external-dicts-dict-structure/) stored in the dict.
   - `lifetime`: The dict [lifetime](https://clickhouse.com/docs/en/sql-reference/dictionaries/external-dictionaries/external-dicts-dict-lifetime/).

### External dicts { #external-dict }

CHYT supports all `layout`, `structure`, and `lifetime` settings of regular ClickHouse.

{% note info "Note" %}

You can specify sources from original ClickHouse as the `source`, but they are not guaranteed to work. In particular, sources that require networking should technically work, but may face a lack of network access in practice.

{% endnote %}

There is also an additional data source type in which you can use static tables in {{product-name}}. This type is called `yt` and has one parameter:

- `path`: The path to the static table on the cluster which will serve as a data source for the dict.

As an example of connecting a {{product-name}} table as an external ClickHouse dict, you can look at:
- The CHYT [configuration example](../../../../../user-guide/data-processing/chyt/reference/configuration.md#configuration_example) that connects this table as an external ClickHouse dict.
- Example of using an external dict in queries:

```sql
select dictGet('OS', 'OS', toUInt64(38)) as os_name,
    dictGetHierarchy('OS', toUInt64(38)) as hierarchy,
    dictGetChildren('OS', toUInt64(101)) as children
```

## Configuration example { #configuration_example }

Below is an example of the complete configuration that can be specified with a `set-speclet` command.

```json
{
    instance_count = 1;
    query_settings = {
        extremes = 1;
    };
    yt_config = {
        subquery = {
            max_data_weight_per_subquery = 1000000000000;
        };
    };
    clickhouse_config = {
        dictionaries = [
            {
                name = OS;
                layout = {flat = {}};
                structure = {
                    id = {name = Id};
                    attribute = [
                        {
                            name = "OS";
                            type = "Nullable(String)";
                            null_value = "NULL";
                        };
                        {
                            name = "ParentId";
                            type = "Nullable(UInt64)";
                            null_value = "NULL";
                            hierarchical = %true;
                        };
                        {
                            name = "RootId";
                            type = "Nullable(UInt64)";
                            null_value = "NULL";
                        };
                    ];
                };
                lifetime = 0;
                source = {yt = {path = "//sys/clickhouse/dictionaries/OS"}};
            };
        ];
    };
}
```