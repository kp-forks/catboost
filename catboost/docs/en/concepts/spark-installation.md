# {{ catboost-spark }} installation

## Requirements

* Linux or macOS with a CPU architecture `x86_64` or `aarch64`/`arm64`. Windows support in progress.
* Apache Spark 3.0 - 3.5
* Scala 2.12 or 2.13 (corresponding to versions supported by the particular Spark version)

  {% cut "Previous versions" %}

  CatBoost versions before 1.2.8 supported Apache Spark versions 2.3 - 2.4 as well.
  CatBoost packages for these Apache Spark versions also had Scala 2.11 compatible variants.

  {% endcut %}

## Choose installation method

It is generally recommended to use stable releases available as Maven artifacts from [Maven central](https://search.maven.org/search?q=catboost-spark):

* [For Maven projects](../installation/spark-installation-maven.md)
* [For sbt projects](../installation/spark-installation-sbt.md)
* [For PySpark](../installation/spark-installation-pyspark.md)

You can also [build from source using Maven](../installation/spark-installation-build-from-source-maven.md).
