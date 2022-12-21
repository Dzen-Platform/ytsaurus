package tech.ytsaurus.client.operations;

import java.util.List;
import java.util.Optional;

import javax.annotation.Nullable;

import tech.ytsaurus.client.TransactionalClient;
import tech.ytsaurus.core.cypress.YPath;
import tech.ytsaurus.ysontree.YTreeBuilder;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;

/**
 * Spec of the map operation.
 *
 * @see <a href="https://yt.yandex-team.ru/docs/description/mr/map">
 * map documentation
 * </a>
 */
@NonNullApi
@NonNullFields
public class MapSpec extends SimpleUserOperationSpecBase implements Spec {
    private final UserJobSpec mapperSpec;
    @Nullable
    private final JobIo jobIo;

    /**
     * Construct map spec from input and output tables and command with other options set by defaults.
     */
    public MapSpec(
            List<YPath> inputTables,
            List<YPath> outputTables,
            String command) {
        this(builder()
                .setInputTables(inputTables)
                .setOutputTables(outputTables)
                .setMapperCommand(command));
    }

    /**
     * Construct map spec from input and output tables and mapper with other options set by defaults.
     */
    public MapSpec(
            List<YPath> inputTables,
            List<YPath> outputTables,
            Mapper<?, ?> mapper) {
        this(builder()
                .setInputTables(inputTables)
                .setOutputTables(outputTables)
                .setMapperSpec(new MapperSpec(mapper)));
    }

    /**
     * Construct map spec from input and output tables and mapperSpec with other options set by defaults.
     */
    public MapSpec(
            List<YPath> inputTables,
            List<YPath> outputTables,
            UserJobSpec mapperSpec) {
        this(builder()
                .setInputTables(inputTables)
                .setOutputTables(outputTables)
                .setMapperSpec(mapperSpec));
    }

    protected <T extends BuilderBase<T>> MapSpec(BuilderBase<T> builder) {
        super(builder);

        if (builder.mapperSpec == null) {
            throw new RuntimeException("mapper is not set");
        }
        mapperSpec = builder.mapperSpec;

        if (mapperSpec instanceof MapperOrReducerSpec && ((MapperOrReducerSpec) mapperSpec).trackIndices()) {
            this.jobIo = (builder.jobIo == null ? JobIo.builder() : builder.jobIo.toBuilder())
                    .setEnableRowIndex(true)
                    .setEnableTableIndex(true)
                    .build();
        } else {
            this.jobIo = builder.jobIo;
        }
    }

    /**
     * @see Builder#setJobIo
     */
    public Optional<JobIo> getJobIo() {
        return Optional.ofNullable(jobIo);
    }

    /**
     * @see Builder#setMapperSpec
     */
    public UserJobSpec getMapperSpec() {
        return mapperSpec;
    }

    /**
     * Create yson map spec to transfer to YT.
     */
    @Override
    public YTreeBuilder prepare(YTreeBuilder builder, TransactionalClient yt, SpecPreparationContext context) {
        SpecUtils.createOutputTables(yt, getOutputTables(), getOutputTableAttributes());
        return builder.beginMap()
                .apply(b -> SpecUtils.addMapperOrReducerTitle(b, mapperSpec))
                .key("mapper").apply(b -> mapperSpec.prepare(b, yt, context, getOutputTables().size()))
                .when(jobIo != null, b -> b.key("job_io").value(jobIo.prepare()))
                .apply(b -> dumpToSpec(b, context))
                .endMap();
    }

    /**
     * Construct empty builder for map spec.
     */
    public static BuilderBase<?> builder() {
        return new Builder();
    }

    /**
     * Builder for {@link MapSpec}
     */
    protected static class Builder extends BuilderBase<Builder> {
        @Override
        protected Builder self() {
            return this;
        }
    }

    /**
     * BuilderBase was taken out because there is another client
     * which we need to support too and which use the same MapSpec class.
     */
    @NonNullApi
    @NonNullFields
    public abstract static class BuilderBase<T extends BuilderBase<T>> extends SimpleUserOperationSpecBase.Builder<T> {
        private @Nullable
        UserJobSpec mapperSpec;
        private @Nullable
        JobIo jobIo;

        protected BuilderBase() {
        }

        /**
         * Construct {@link MapSpec} instance.
         */
        public MapSpec build() {
            return new MapSpec(this);
        }

        /**
         * Set mapper spec.
         *
         * @see MapperSpec
         * @see CommandSpec
         */
        public T setMapperSpec(UserJobSpec mapperSpec) {
            this.mapperSpec = mapperSpec;
            return self();
        }

        /**
         * Set mapper command.
         */
        public T setMapperCommand(String command) {
            return setMapperSpec(new CommandSpec(command));
        }

        /**
         * Set job I/O options.
         *
         * @see JobIo
         */
        public T setJobIo(@Nullable JobIo jobIo) {
            this.jobIo = jobIo;
            return self();
        }
    }
}
