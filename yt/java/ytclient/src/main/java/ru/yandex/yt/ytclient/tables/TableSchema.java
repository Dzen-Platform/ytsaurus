package ru.yandex.yt.ytclient.tables;

import java.util.AbstractList;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.stream.Collectors;

import ru.yandex.inside.yt.kosher.impl.ytree.builder.YTree;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;
import ru.yandex.yt.ytclient.ytree.YTreeConvertible;

/**
 * TTableSchema (yt/ytlib/table_client/schema.h)
 */
public class TableSchema implements YTreeConvertible {
    private final List<ColumnSchema> columns;
    private final boolean strict;
    private final boolean uniqueKeys;
    private final Map<String, Integer> columnsByName = new HashMap<>();
    private final int keyColumnsCount;
    private final boolean isKeysSchema;
    private final boolean isValuesSchema;
    private final boolean isWriteSchema;
    private final boolean isLookupSchema;

    private TableSchema(List<ColumnSchema> columns, boolean strict, boolean uniqueKeys) {
        this.columns = Objects.requireNonNull(columns);
        this.strict = strict;
        this.uniqueKeys = uniqueKeys;
        int keyColumnsCount = 0;
        boolean isKeysSchema = true;
        boolean isValuesSchema = true;
        boolean isWriteSchema = true;
        boolean isLookupSchema = true;
        for (int index = 0; index < columns.size(); index++) {
            ColumnSchema column = columns.get(index);
            if (columnsByName.containsKey(column.getName())) {
                throw new IllegalArgumentException("Duplicate column " + column.getName());
            }
            columnsByName.put(column.getName(), index);
            if (column.getLock() != null && column.getLock().isEmpty()) {
                throw new IllegalArgumentException("Column " + column.getName() + ": lock name cannot be empty");
            }
            if (column.getGroup() != null && column.getGroup().isEmpty()) {
                throw new IllegalArgumentException("Column " + column.getName() + ": group name cannot be empty");
            }
            if (column.getSortOrder() != null) {
                if (column.getAggregate() != null) {
                    throw new IllegalArgumentException("Key column " + column.getName() + " cannot be aggregated");
                }
                if (index != keyColumnsCount) {
                    throw new IllegalArgumentException("Key columns must form a prefix of schema");
                }
                ++keyColumnsCount;
                isValuesSchema = false;
            } else {
                isKeysSchema = false;
                isLookupSchema = false;
            }
            if (column.getExpression() != null) {
                isWriteSchema = false;
                isLookupSchema = false;
            }
        }
        this.keyColumnsCount = keyColumnsCount;
        if (uniqueKeys && keyColumnsCount == 0) {
            throw new IllegalArgumentException("Cannot set uniqueKeys on schemas without key columns");
        }
        this.isKeysSchema = isKeysSchema;
        this.isValuesSchema = isValuesSchema;
        this.isWriteSchema = isWriteSchema;
        this.isLookupSchema = isLookupSchema;
    }

    public List<ColumnSchema> getColumns() {
        return Collections.unmodifiableList(columns);
    }

    public int getKeyColumnsCount() {
        return keyColumnsCount;
    }

    public int getColumnsCount() {
        return columns.size();
    }

    public boolean isStrict() {
        return strict;
    }

    public boolean isUniqueKeys() {
        return uniqueKeys;
    }

    /**
     * Возвращает true, если схема содержит только ключевые колонки
     */
    public boolean isKeysSchema() {
        return isKeysSchema;
    }

    /**
     * Возвращает true, если схема содержит только неключевые колонки
     */
    public boolean isValuesSchema() {
        return isValuesSchema;
    }

    /**
     * Возвращает true, если схема подходит для записи данных (отсутствуют вычисляемые колонки)
     */
    public boolean isWriteSchema() {
        return isWriteSchema;
    }

    /**
     * Возвращает true, если схема подходит для поиска данных (только невычисляемые ключевые колонки)
     */
    public boolean isLookupSchema() {
        return isLookupSchema;
    }

    /**
     * Возвращает индекс колонки с именем name или -1, если такой колонки нет
     */
    public int findColumn(String name) {
        Integer index = columnsByName.get(name);
        if (index == null) {
            return -1;
        }
        return index;
    }

    /**
     * Возвращает схему колонки по ее индексу или null
     *
     * @param index индекс колонки
     * @return описание колонки или null, если такого индекса нет
     */
    public ColumnSchema getColumnSchema(int index) {
        if (index >= 0 && index < columns.size()) {
            return columns.get(index);
        } else {
            return null;
        }
    }

    /**
     * Возаращает имя колонки с индексом index
     */
    public String getColumnName(int index) {
        final ColumnSchema column = getColumnSchema(index);
        if (column != null) {
            return column.getName();
        } else {
            // Пока есть проблемы, что id в rowset'ах не соответствуют схеме
            // На случай несоответствий возвращаем хоть какое-то имя
            return "<unknown_" + index + ">";
        }
    }

    /**
     * Возвращает тип колонки с индексом index
     */
    public ColumnValueType getColumnType(int index) {
        final ColumnSchema column = getColumnSchema(index);
        if (column != null) {
            return column.getType();
        } else {
            // Пока есть проблемы, что id в rowset'ах не соответствуют схеме
            // На случай несоответствий возвращаем хоть какой-то тип
            return ColumnValueType.ANY;
        }
    }

    /**
     * Возвращает список с именами всех колонок
     */
    public List<String> getColumnNames() {
        return new AbstractList<String>() {
            @Override
            public String get(int index) {
                return columns.get(index).getName();
            }

            @Override
            public int size() {
                return columns.size();
            }
        };
    }

    /**
     * Конвертирует схему в форму для записи данных (вырезаются все вычисляемые колонки)
     */
    public TableSchema toWrite() {
        if (isWriteSchema) {
            return this;
        }
        List<ColumnSchema> newColumns = new ArrayList<>();
        for (ColumnSchema column : columns) {
            if (column.getExpression() == null) {
                newColumns.add(column);
            }
        }
        return new TableSchema(newColumns, strict, uniqueKeys);
    }

    /**
     * Конвертирует схему в форму для поиска данных (только ключевые невычисляемые колонки)
     */
    public TableSchema toLookup() {
        if (isLookupSchema) {
            return this;
        }
        List<ColumnSchema> newColumns = new ArrayList<>();
        for (ColumnSchema column : columns) {
            if (column.getExpression() == null && column.getSortOrder() != null) {
                newColumns.add(column);
            }
        }
        return new TableSchema(newColumns, strict, uniqueKeys);
    }

    /**
     * Конвертирует схему в форму для удаления данных (аналогично toLookup)
     */
    public TableSchema toDelete() {
        return toLookup();
    }

    /**
     * Возвращает копию схемы, состоящую только из ключевых колонок
     */
    public TableSchema toKeys() {
        if (isKeysSchema) {
            return this;
        }
        List<ColumnSchema> newColumns = new ArrayList<>();
        for (ColumnSchema column : columns) {
            if (column.getSortOrder() != null) {
                newColumns.add(column);
            }
        }
        return new TableSchema(newColumns, strict, uniqueKeys);
    }

    /**
     * Возвращает копию схемы, состоящую только из неключевых колонок
     */
    public TableSchema toValues() {
        if (isValuesSchema) {
            return this;
        }
        List<ColumnSchema> newColumns = new ArrayList<>();
        for (ColumnSchema column : columns) {
            if (column.getSortOrder() == null) {
                newColumns.add(column);
            }
        }
        return new TableSchema(newColumns, strict, false);
    }

    /**
     * Возвращает копию схему с uniqueKeys=true
     */
    public TableSchema toUniqueKeys() {
        if (uniqueKeys) {
            return this;
        }
        return new TableSchema(columns, strict, true);
    }

    @Override
    public YTreeNode toYTree() {
        return YTree.builder().beginAttributes()
                .key("strict").value(strict)
                .key("unique_keys").value(uniqueKeys)
                .endAttributes()
                .value(columns.stream().map(ColumnSchema::toYTree).collect(Collectors.toList()))
                .build();
    }

    public static TableSchema fromYTree(YTreeNode node) {
        List<YTreeNode> list = node.listNode().asList();
        List<ColumnSchema> columns = new ArrayList<>();
        for (YTreeNode columnNode : list) {
            columns.add(ColumnSchema.fromYTree(columnNode));
        }
        boolean strict = node.getAttribute("strict").map(YTreeNode::boolValue).getOrElse(true);
        boolean uniqueKeys = node.getAttribute("unique_keys").map(YTreeNode::boolValue).getOrElse(false);
        return new TableSchema(columns, strict, uniqueKeys);
    }

    @Override
    public String toString() {
        return toYTree().toString();
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (!(o instanceof TableSchema)) {
            return false;
        }

        TableSchema that = (TableSchema) o;

        if (strict != that.strict) {
            return false;
        }
        if (uniqueKeys != that.uniqueKeys) {
            return false;
        }
        return columns.equals(that.columns);
    }

    @Override
    public int hashCode() {
        int result = columns.hashCode();
        result = 31 * result + (strict ? 1 : 0);
        result = 31 * result + (uniqueKeys ? 1 : 0);
        return result;
    }

    public static class Builder {
        private final List<ColumnSchema> columns = new ArrayList<>();
        private String lock = null;
        private String group = null;
        private boolean strict = true;
        private boolean uniqueKeys = true;
        private int keysCount = 0;

        public Builder addKey(String name, ColumnValueType type) {
            return add(new ColumnSchema(name, type, ColumnSortOrder.ASCENDING, lock, null, null, group, false));
        }

        public Builder addKeyExpression(String name, ColumnValueType type, String expression) {
            return add(new ColumnSchema(name, type, ColumnSortOrder.ASCENDING, lock, expression, null, group, false));
        }

        public Builder addValue(String name, ColumnValueType type) {
            return add(new ColumnSchema(name, type, null, lock, null, null, group, false));
        }

        public Builder addValueAggregate(String name, ColumnValueType type, String aggregate) {
            return add(new ColumnSchema(name, type, null, lock, null, aggregate, group, false));
        }

        public Builder add(ColumnSchema column) {
            columns.add(Objects.requireNonNull(column));
            if (column.getSortOrder() != null) {
                ++keysCount;
            }
            return this;
        }

        public Builder setLock(String lock) {
            this.lock = lock;
            return this;
        }

        public Builder clearLock() {
            this.lock = null;
            return this;
        }

        public Builder setGroup(String group) {
            this.group = group;
            return this;
        }

        public Builder clearGroup() {
            this.group = null;
            return this;
        }

        public Builder setStrict(boolean strict) {
            this.strict = strict;
            return this;
        }

        public Builder setUniqueKeys(boolean uniqueKeys) {
            this.uniqueKeys = uniqueKeys;
            return this;
        }

        public TableSchema build() {
            return new TableSchema(new ArrayList<>(columns), strict, uniqueKeys);
        }
    }
}
