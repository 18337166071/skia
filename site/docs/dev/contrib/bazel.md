
---
title: "Using Bazel"
linkTitle: "Using Bazel"

---

## Overview

Skia is currently migrating towards using [Bazel](https://bazel.build/) as a build system, due to
the ability to more tightly control what and how gets built.

When referring to a file in this doc, we use
[Bazel label notation](https://bazel.build/concepts/labels), so to refer the file located at
`$SKIA_ROOT/docs/examples/Arc.cpp`, we would say `//docs/examples/Arc.cpp`.

## Learning more about Bazel
The Bazel docs are quite good. Suggested reading order if you are new to Bazel:
 - [Getting Started with Bazel and C++](https://bazel.build/tutorials/cpp)
 - [WORKSPACE.bazel and external deps](https://bazel.build/docs/external)
 - [Targets and Labels](https://bazel.build/concepts/labels)
 - [Understanding a build](https://bazel.build/docs/build)
 - [Configuration with .bazelrc files](https://bazel.build/docs/bazelrc)

Googlers, check out [go/bazel-bites](http://go/bazel-bites) for more tips.

## Building with Bazel

All this assumes you have [downloaded Skia](/docs/user/download), especially having synced the
third_party deps using `./tools/git-sync-deps`.

### Linux Hosts (you are running Bazel on a Linux machine)
You can run a command like:
```
bazel build //example:hello_world_gl --config=clang_linux
```

This uses the `clang_linux` configuration (defined in `//.bazelrc`), which is a hermetic C++
toolchain we put together to compile Skia on a Linux host (implementation is in `//toolchain`.
It builds the _target_ defined in `//examples/BUILD.bazel` named "hello_world_gl", which uses
the `sk_app` framework we designed to make simple applications using Skia.

Bazel will put this executable in `//bazel-bin/example/hello_world_gl` and tell you it did so in
the logs. You can run this executable yourself, or have Bazel run it by modifying the command to
be:
```
bazel run //example:hello_world_gl --config=clang_linux
```

If you want to pass one or more flags to `bazel run`, add them on the end after a `--` like:
```
bazel run //example:hello_world_gl --config=clang_linux -- --flag_one=apple --flag_two=cherry
```

## .bazelrc Tips
You should make a [.bazelrc file](https://bazel.build/docs/bazelrc) in your home directory where
you can specify settings that apply only to you. These can augment or replace the ones we define
in `//.bazelrc`

You may want some or all of the following entries in your `~/.bazelrc` file.

### Build Skia faster locally
Many Linux machines have a [RAM disk mounted at /dev/shm](https://www.cyberciti.biz/tips/what-is-devshm-and-its-practical-usage.html)
and using this as the location for the Bazel sandbox can dramatically improve compile times because
[sandboxing](https://bazel.build/docs/sandboxing) has been observed to be I/O intensive.

Add the following to `~/.bazelrc` if you have a `/dev/shm` partition that is 4+ GB big. 
```
build:clang --sandbox_base=/dev/shm
```

### Authenticate to RBE on a Linux VM
We are in the process of setting up Remote Build Execution (RBE) for Bazel. Some users have reported
errors when trying to use RBE (via `--config=linux-rbe`) on Linux VMs such as:
```
ERROR: Failed to query remote execution capabilities: 
Error code 404 trying to get security access token from Compute Engine metadata for the default
service account. This may be because the virtual machine instance does not have permission
scopes specified. It is possible to skip checking for Compute Engine metadata by specifying the
environment  variable NO_GCE_CHECK=true.
```
For instances where it is not possible to set the `cloud-platform` scope
[on the VM](https://skia-review.googlesource.com/c/skia/+/525577), one can directly link to their
GCP credentials by adding the following to `~/.bazelrc` (substituting their username for &lt;user>)
after logging in via `gcloud auth login`:
```
build:remote --google_credentials=/usr/local/google/home/<user>/.config/gcloud/application_default_credentials.json
```