import sbt._
import spyt.SparkForkVersion.sparkForkVersion

object Dependencies {
  lazy val circeVersion = "0.12.3"
  lazy val circeYamlVersion = "0.12.0"
  lazy val scalatestVersion = "3.1.0"
  lazy val sparkVersion = "3.2.2"
  lazy val ytsaurusClientVersion = "1.2.0"
  lazy val slf4jVersion = "1.7.28"
  lazy val scalatraVersion = "2.7.0"
  lazy val mockitoVersion = "1.14.4"
  lazy val arrowVersion = "0.17.1"

  lazy val circe = ("io.circe" %% "circe-yaml" % circeYamlVersion) +: Seq(
    "io.circe" %% "circe-core",
    "io.circe" %% "circe-generic",
    "io.circe" %% "circe-parser"
  ).map(_ % circeVersion)

  lazy val mockito = Seq(
    "org.mockito" %% "mockito-scala-scalatest" % mockitoVersion % Test,
    "org.mockito" %% "mockito-scala" % mockitoVersion % Test
  )

  lazy val dockerTest = Seq(
    "com.whisk" %% "docker-testkit-scalatest" % "0.9.9" % Test,
    "com.whisk" %% "docker-testkit-impl-docker-java" % "0.9.9" % Test,
    "com.kohlschutter.junixsocket" % "junixsocket-common" % "2.6.1" % Test,
    "com.kohlschutter.junixsocket" % "junixsocket-native-common" % "2.6.1" % Test
  ).map(_ excludeAll(
    ExclusionRule(organization = "io.netty")
  ))

  lazy val testDeps = Seq(
    "org.scalacheck" %% "scalacheck" % "1.14.3" % Test,
    "org.scalactic" %% "scalactic" % scalatestVersion,
    "org.scalatest" %% "scalatest" % scalatestVersion % Test,
    "org.scalatestplus" %% "scalacheck-1-14" % "3.1.0.0" % Test
  ) ++ mockito ++ dockerTest

  lazy val itTestDeps = Seq(
    "org.scalacheck" %% "scalacheck" % "1.14.1" % "it,test",
    "org.scalatest" %% "scalatest" % scalatestVersion % "it,test"
  )

  lazy val scalatraTestDeps = Seq(
    "org.scalatra" %% "scalatra-scalatest" % scalatraVersion % "it" excludeAll
      ExclusionRule(organization = "org.scalatest")
  )

  lazy val spark = Seq(
    "tech.ytsaurus.spark" %% "spark-core",
    "tech.ytsaurus.spark" %% "spark-sql"
  ).map(_ % sparkForkVersion).map(_ excludeAll
    ExclusionRule(organization = "org.apache.httpcomponents")
  ).map(_ % Provided)

  lazy val sparkRuntime = Seq(
    "tech.ytsaurus.spark" %% "spark-core",
    "tech.ytsaurus.spark" %% "spark-sql"
  ).map(_ % sparkForkVersion).map(_ excludeAll
    ExclusionRule(organization = "org.apache.httpcomponents")
  ).excludeLogging

  lazy val ytsaurusClient = Seq(
    "tech.ytsaurus" % "ytsaurus-client" % ytsaurusClientVersion excludeAll ExclusionRule(organization = "io.netty"),
    "io.netty" % "netty-all" % "4.1.68.Final"
  ).map(_ excludeAll(
    ExclusionRule(organization = "com.fasterxml.jackson.core"),
    ExclusionRule(organization = "org.apache.commons"),
    ExclusionRule(organization = "com.google.code.findbugs", name = "jsr305")
  )).excludeLogging

  lazy val grpc = Seq(
    "io.grpc" % "grpc-netty" % scalapb.compiler.Version.grpcJavaVersion,
    "com.thesamet.scalapb" %% "scalapb-runtime-grpc" % scalapb.compiler.Version.scalapbVersion
  )

  lazy val scaldingArgs = Seq(
    "com.twitter" %% "scalding-args" % "0.17.4"
  )

  lazy val py4j = Seq(
    "net.sf.py4j" % "py4j" % "0.10.9"
  )

  lazy val logging = Seq(
    "org.slf4j" % "slf4j-log4j12",
    "org.slf4j" % "slf4j-api",
    "org.slf4j" % "jul-to-slf4j"
  ).map(_ % slf4jVersion) ++ Seq(
    "log4j" % "log4j" % "1.2.17"
  ) ++ Seq(
    "net.logstash.log4j" % "jsonevent-layout" % "1.7"
  )

  lazy val scalatra = Seq(
    "org.scalatra" %% "scalatra" % scalatraVersion,
    "org.eclipse.jetty" % "jetty-webapp" % "9.2.19.v20160908" % Compile,
    "javax.servlet" % "javax.servlet-api" % "3.1.0" % Provided
  )

  lazy val sttp = Seq(
    "com.softwaremill.sttp.client" %% "core" % "2.1.4"
  )

  lazy val metrics = Seq(
    "io.dropwizard.metrics" % "metrics-core" % "4.2.0" % Compile
  )

  implicit class RichDependencies(deps: Seq[ModuleID]) {
    def excludeLogging: Seq[ModuleID] = deps.map(_.excludeAll(
      ExclusionRule(organization = "org.slf4j"),
      ExclusionRule(organization = "log4j")
    ))
  }
}
