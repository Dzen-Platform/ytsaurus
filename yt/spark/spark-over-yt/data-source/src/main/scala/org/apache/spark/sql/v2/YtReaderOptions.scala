package org.apache.spark.sql.v2

import org.apache.spark.sql.internal.SQLConf
import org.apache.spark.sql.types._
import tech.ytsaurus.spyt.format.conf.{SparkYtConfiguration, YtTableSparkSettings}

object YtReaderOptions {
  private val unsupportedTypes: Set[DataType] = Set(DateType, TimestampType, FloatType)

  def optimizedForScan(options: Map[String, String]): Boolean = {
    import tech.ytsaurus.spyt.fs.conf._
    options.getYtConf(YtTableSparkSettings.OptimizedForScan).exists(identity)
  }

  def canReadBatch(dataSchema: StructType, optimizedForScan: Boolean, arrowEnabled: Boolean): Boolean = {
    (optimizedForScan && arrowEnabled && arrowSchemaSupported(dataSchema)) || dataSchema.isEmpty
  }

  def canReadBatch(dataSchema: StructType, options: Map[String, String], conf: SQLConf): Boolean = {
    (optimizedForScan(options) && arrowEnabled(options, conf) && arrowSchemaSupported(dataSchema)) || dataSchema.isEmpty
  }

  def arrowEnabled(options: Map[String, String], conf: SQLConf): Boolean = {
    import tech.ytsaurus.spyt.fs.conf._
    options.ytConf(YtTableSparkSettings.ArrowEnabled) && conf.ytConf(SparkYtConfiguration.Read.ArrowEnabled) &&
      !options.getYtConf(YtTableSparkSettings.KeyPartitioned).exists(identity)
  }

  def supportBatch(canReadBatch: Boolean, resultSchema: StructType, conf: SQLConf): Boolean = {
    canReadBatch && conf.wholeStageEnabled &&
      resultSchema.length <= conf.wholeStageMaxNumFields &&
      resultSchema.forall(_.dataType.isInstanceOf[AtomicType])
  }

  def supportBatch(dataSchema: StructType, options: Map[String, String], conf: SQLConf): Boolean = {
    supportBatch(canReadBatch(dataSchema, options, conf), dataSchema, conf)
  }

  private def arrowSchemaSupported(dataSchema: StructType): Boolean = {
    dataSchema.fields.forall(f => !unsupportedTypes.contains(f.dataType))
  }
}
