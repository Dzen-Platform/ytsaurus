package ru.yandex.yt.ytclient.proxy.request;

import java.util.Arrays;
import java.util.List;

import ru.yandex.yt.rpcproxy.TAttributeKeys;

public class ColumnFilter {
    private Boolean all;
    private List<String> columns;

    static public ColumnFilter of(String... columns) {
        ColumnFilter result = new ColumnFilter();
        result.columns = Arrays.asList(columns);
        return result;
    }

    public ColumnFilter() {}

    public ColumnFilter(boolean all, List<String> columns) {
        this.all = all;
        this.columns = columns;
    }

    public ColumnFilter setAll(boolean all) {
        this.all = all;
        return this;
    }

    public ColumnFilter setColumns(List<String> columns) {
        this.columns = columns;
        return this;
    }

    public TAttributeKeys.Builder writeTo(TAttributeKeys.Builder builder) {
        if (all != null) {
            builder.setAll(all);
        }
        builder.addAllColumns(columns);
        return builder;
    }

    @Override
    public String toString() {
        return columns.toString();
    }
}
