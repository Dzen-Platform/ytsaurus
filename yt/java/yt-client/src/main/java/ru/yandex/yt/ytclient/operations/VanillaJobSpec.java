package ru.yandex.yt.ytclient.operations;

import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

import tech.ytsaurus.core.cypress.YPath;
import tech.ytsaurus.ysontree.YTree;
import tech.ytsaurus.ysontree.YTreeBuilder;
import tech.ytsaurus.ysontree.YTreeNode;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.ytclient.proxy.TransactionalClient;

@NonNullApi
@NonNullFields
public class VanillaJobSpec extends MapperOrReducerSpec implements Spec {
    private final List<YPath> outputTablePaths;

    protected VanillaJobSpec(Builder builder) {
        super(VanillaMain.class, builder);
        outputTablePaths = builder.outputTablePaths;
    }

    @Override
    public YTreeBuilder prepare(YTreeBuilder builder, TransactionalClient yt, SpecPreparationContext context,
                                int outputTableCount) {
        return prepare(builder, yt, context);
    }

    @Override
    public YTreeBuilder prepare(YTreeBuilder builder, TransactionalClient yt, SpecPreparationContext context) {
        if (outputTablePaths.isEmpty()) {
            return super.prepare(builder, yt, context, 0);
        } else {
            YTreeBuilder prepare = super.prepare(YTree.builder(), yt, context, outputTablePaths.size());
            Map<String, YTreeNode> node = new HashMap<>(prepare.build().asMap());

            node.put("output_table_paths", YTree.builder()
                    .value(outputTablePaths.stream().map(YPath::toTree).collect(Collectors.toList()))
                    .build());
            return builder.value(node);
        }
    }

    public static Builder builder(VanillaJob<?> job) {
        Builder builder = new Builder();
        builder.setJob(job);
        return builder;
    }

    public static Builder builder() {
        return new Builder();
    }

    @NonNullApi
    @NonNullFields
    public static class Builder extends MapperOrReducerSpec.Builder<Builder> {
        private List<YPath> outputTablePaths = Arrays.asList();

        Builder() {
            // Vanilla jobs require setting job count
            setJobCount(1);
        }

        @Override
        public VanillaJobSpec build() {
            if (getUserJob() == null) {
                throw new IllegalStateException("Job is required and has no default value");
            }
            return new VanillaJobSpec(this);
        }

        @Override
        protected Builder self() {
            return this;
        }

        public Builder setJob(VanillaJob<?> job) {
            setUserJob(job);
            return this;
        }

        /**
         * Sets job_count.
         * <p>
         * This setter overriden from our base class performs additional checks.
         * Vanilla job_count must always be set and its value must be >= 0.
         */
        @Override
        public Builder setJobCount(Integer jobCount) {
            if (jobCount < 1) {
                throw new IllegalArgumentException("Vanilla's job_count must be >= 1");
            }
            return super.setJobCount(jobCount);
        }

        public Builder setOutputTablePaths(List<YPath> outputTablePaths) {
            this.outputTablePaths = outputTablePaths;
            return this;
        }
    }
}
