package ru.yandex.yt.ytclient.operations;

import java.time.Duration;
import java.time.Instant;
import java.time.format.DateTimeParseException;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.stream.Collectors;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.inside.yt.kosher.ytree.YTreeListNode;
import ru.yandex.inside.yt.kosher.ytree.YTreeMapNode;
import ru.yandex.inside.yt.kosher.ytree.YTreeNode;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.ytclient.proxy.ApiServiceClient;
import ru.yandex.yt.ytclient.proxy.request.AbortOperation;
import ru.yandex.yt.ytclient.proxy.request.GetJobStderr;
import ru.yandex.yt.ytclient.proxy.request.GetOperation;
import ru.yandex.yt.ytclient.proxy.request.JobResult;
import ru.yandex.yt.ytclient.proxy.request.JobState;
import ru.yandex.yt.ytclient.proxy.request.ListJobs;

@NonNullApi
@NonNullFields
public class OperationImpl implements Operation {
    private static final Logger logger = LoggerFactory.getLogger(OperationImpl.class);

    private final GUID id;
    private final ApiServiceClient client;
    private final ScheduledExecutorService executorService;
    private final Duration pingPeriod;
    private final CompletableFuture<Void> watchResult = new CompletableFuture<>();

    private Instant previousBriefProgressBuildTime;

    public OperationImpl(
            GUID id,
            ApiServiceClient client,
            ScheduledExecutorService executorService,
            Duration pingPeriod
    ) {
        this.id = id;
        this.client = client;
        this.executorService = executorService;
        this.pingPeriod = pingPeriod;
        this.previousBriefProgressBuildTime = Instant.now();
    }

    @Override
    public GUID getId() {
        return id;
    }

    @Override
    public CompletableFuture<OperationStatus> getStatus() {
        return getOperation("state")
                .thenApply(node -> OperationStatus.R.fromName(node.mapNode().getOrThrow("state").stringValue()));
    }

    @Override
    public CompletableFuture<YTreeNode> getResult() {
        return getOperation("result")
                .thenApply(node -> node.mapNode().getOrThrow("result"));
    }

    @Override
    public CompletableFuture<Void> watch() {
        executorService.schedule(this::watchImpl, pingPeriod.toNanos(), TimeUnit.NANOSECONDS);
        return watchResult;
    }

    @Override
    public CompletableFuture<Void> abort() {
        return client.abortOperation(new AbortOperation(id));
    }

    private CompletableFuture<YTreeNode> getOperation(String attribute) {
        return client.getOperation(new GetOperation(id).addAttribute(attribute));
    }

    private void watchImpl() {
        logger.debug("Operation's watch iteration was started (OperationId: {})", id);
        client.getOperation(new GetOperation(id)
                        .addAttribute("state")
                        .addAttribute("brief_progress")
                        .addAttribute("operation_type"))
                .thenApply(this::getAndLogStatus)
                .thenCompose(status -> {
                    if (status.isFinished()) {
                        return getAndLogFailedJobs(id).handle((unused, ex) -> {
                            if (ex != null) {
                                logger.warn("Cannot get failed jobs info", ex);
                            }

                            watchResult.complete(null);
                            return null;
                        });
                    }
                    return CompletableFuture.completedFuture(null);
                }).handle((unused, ex) -> {
                    if (!watchResult.isDone()) {
                        executorService.schedule(this::watchImpl, pingPeriod.toNanos(), TimeUnit.NANOSECONDS);
                    }
                    return null;
                });
    }

    private OperationStatus getAndLogStatus(YTreeNode getOperationResult) {
        Map<String, YTreeNode> attrs = getOperationResult.asMap();
        String state = attrs.get("state").stringValue();
        OperationStatus status;
        try {
            status = OperationStatus.R.fromName(state);
        } catch (IllegalArgumentException e) {
            status = OperationStatus.UNKNOWN;
        }

        String statusDescription = state;
        if (attrs.containsKey("brief_progress") && attrs.get("brief_progress").mapNode().containsKey("jobs")) {
            YTreeMapNode briefProgress = attrs.get("brief_progress").mapNode();
            YTreeMapNode progress = briefProgress.getOrThrow("jobs").mapNode();
            Instant buildTime;
            try {
                buildTime = Instant.parse(briefProgress.getOrThrow("build_time").stringValue());
            } catch (DateTimeParseException e) {
                buildTime = previousBriefProgressBuildTime; // do not log unless received build_time
            }

            if (buildTime.compareTo(previousBriefProgressBuildTime) > 0 && progress.containsKey("total")) {
                StringBuilder sb = new StringBuilder();
                if (progress.containsKey("running")) {
                    sb.append("running ").append(progress.getOrThrow("running").longValue()).append(", ");
                }
                if (progress.containsKey("completed")) {
                    YTreeNode node = progress.getOrThrow("completed");
                    long count = 0;
                    if (node.isIntegerNode()) {
                        count = node.longValue();
                    } else if (node.isMapNode()) {
                        count = node.mapNode().getLong("total");
                    }
                    sb.append("completed ").append(count).append(", ");
                }
                sb.append("total ").append(progress.getOrThrow("total").longValue());
                sb.append(" (").append(state).append(")");
                statusDescription = sb.toString();
            }
            previousBriefProgressBuildTime = buildTime;
        }
        logger.info("Operation {} ({}): {}", id, attrs.get("operation_type").stringValue(), statusDescription);
        return status;
    }

    private CompletableFuture<Void> getAndLogFailedJobs(GUID operationId) {
        return client.listJobs(new ListJobs(operationId, JobState.Failed, 5L))
                .thenCompose(listJobsResult -> CompletableFuture.allOf(listJobsResult
                            .getJobs()
                            .stream()
                            .map(j -> getAndLogFailedJob(operationId, j))
                            .collect(Collectors.toList()).toArray(new CompletableFuture<?>[0])));
    }

    private CompletableFuture<Void> getAndLogFailedJob(GUID operationId, JobResult job) {
        FailedJobInfo failedJobInfo = new FailedJobInfo(job.getId());
        if (job.getError().isPresent()) {
            traverseInnerErrors(
                    job.getError().get().mapNode(),
                    errorNode -> failedJobInfo.addErrorMessage(errorNode.getString("message")));
        }

        return CompletableFuture.completedFuture(failedJobInfo).thenCompose(
                info -> client
                        .getJobStderr(new GetJobStderr(operationId, job.getId()))
                        .thenApply(getJobStderrResult -> {
                            if (getJobStderrResult.getStderr().isPresent()) {
                                failedJobInfo.setStderr(new String(getJobStderrResult.getStderr().get()));
                            }
                            return failedJobInfo;
                        })
                        .handle((result, ex) -> {
                            if (ex != null) {
                                logger.error("Failed to fetch job details: {}, exception: {}", job.getId(), ex);
                            } else {
                                logger.error(result.toString());
                            }
                            return null;
                        })
        );
    }

    void traverseInnerErrors(YTreeMapNode errorNode, Consumer<YTreeMapNode> errorNodeConsumer) {
        errorNodeConsumer.accept(errorNode);

        Optional<YTreeNode> innerErrors = errorNode.get("inner_errors");
        if (innerErrors.isPresent()) {
            YTreeNode node = innerErrors.get();
            if (node instanceof YTreeListNode) {
                for (YTreeNode innerError : node.asList()) {
                    traverseInnerErrors(innerError.mapNode(), errorNodeConsumer);
                }
            }
        }
    }
}
