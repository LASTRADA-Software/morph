# Self-hosted GitHub Actions runner for LASTRADA-Software/morph.
#
# Base is plain Ubuntu 24.04, matching the ubuntu-24.04 GitHub-hosted label
# the existing ci.yml jobs use. This image intentionally does NOT prebake
# gcc/clang/sccache -- ci.yml's jobs already bootstrap their own toolchain
# via apt-get / apt.llvm.org / sccache-release downloads on every run, so
# duplicating that here would just be a second place to keep in sync.
FROM ubuntu:24.04

ARG RUNNER_VERSION=2.336.0
ARG TARGETARCH=x64
ARG CMAKE_VERSION=3.31.12

ENV DEBIAN_FRONTEND=noninteractive

# GitHub-hosted ubuntu-24.04 runner images bundle ~500 packages that
# ci.yml's jobs rely on without installing themselves (perl, python3,
# binutils' nm, a base gcc/g++/make toolchain, ...). A bare ubuntu:24.04
# Docker image has none of that, so it's listed out explicitly here
# rather than discovered one "command not found" at a time -- python3
# (clang-tidy, coverage script), perl (deprecation-lint,
# test-type-name-lint scripts), binutils' nm (ladder-sanitizers'
# instrumentation check), build-essential for a base gcc/g++/make
# (ci.yml still installs its own pinned gcc-15/clang versions on top).
# cmake is intentionally NOT installed from apt here -- see below.
RUN apt-get update -q && apt-get install -y --no-install-recommends \
        curl \
        ca-certificates \
        git \
        jq \
        sudo \
        software-properties-common \
        gnupg2 \
        lsb-release \
        wget \
        tar \
        gzip \
        python3 \
        perl \
        binutils \
        build-essential \
    && rm -rf /var/lib/apt/lists/*

# Anonymous HTTPS clones from this image are unreliable over HTTP/2, and
# CMakeLists.txt's FetchContent fallback for glaze does exactly one of
# those per configure: GitHub answers the info/refs GET with 200 and then
# the git-upload-pack POST on the same reused HTTP/2 connection with a
# spurious 401 (`www-authenticate: Basic realm="GitHub"`), which git turns
# into `could not read Username for 'https://github.com'` and CMake into
# "Failed to clone repository". Measured here at ~7 failures in 10
# ls-remotes; with -c http.version=HTTP/1.1 (or protocol.version=0) it is
# 10 in 10. It is the client stack, not the network or the IP -- the same
# clone from the Docker host, same public address, is 10 in 10 -- and not
# git's version either: git 2.55 from ppa:git-core/ppa in this image fails
# at the same rate as the distro's 2.43, because both link Ubuntu 24.04's
# libcurl 8.5.0/nghttp2 1.59. There is no newer libcurl to install on
# 24.04, so the fix is to keep git off HTTP/2 entirely. Only
# authenticated requests escape the 401 (git retries with credentials),
# which is why actions/checkout has always worked here and only the
# dependency clone fails. System-wide rather than per-repo: the bug is in
# the HTTP stack, so it applies to every clone any job makes.
RUN git config --system http.version HTTP/1.1

# Ubuntu 24.04's apt package is CMake 3.28.3. GitHub-hosted ubuntu-24.04
# ships a newer CMake from Kitware's own release archive rather than the
# distro package, and that gap is not cosmetic: linux-all-features'
# configure step failed here with "Impossible to link target ...
# because the link item ... has already occurred with the feature
# 'WHOLE_ARCHIVE', which is not allowed" on 3.28.3 -- a false positive in
# $<LINK_LIBRARY:WHOLE_ARCHIVE,...> duplicate-feature detection, an area
# CMake was still actively patching in the 3.28-3.30 range. The same
# configure passes cleanly on GitHub-hosted ubuntu-24.04, confirming this
# is a CMake-version gap, not a real target-graph conflict in
# cmake/morph_add_rung.cmake. Installed directly from Kitware's binary
# release rather than a PPA/apt repo, to pin an exact known-good version.
RUN curl -fsSL -o cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
    && tar -xzf cmake.tar.gz --strip-components=1 -C /usr/local \
    && rm cmake.tar.gz

# clang (installed per-job from apt.llvm.org, not baked into this image)
# uses libstdc++'s headers by default, not its own libc++. Stock
# ubuntu:24.04's default libstdc++ is 13.x, which predates C++23 <print>
# (needs GCC 14+) -- morph's headers use it unconditionally, so any
# clang-only job leg (no gcc install step of its own) fails to compile
# with "fatal error: 'print' file not found" unless a modern-enough
# libstdc++ already exists on the box. ci.yml's own "Install GCC 15" step
# pulls libstdc++-15-dev in as a side effect of gcc-15/g++-15 on legs that
# run it, but clang-only legs (clang-release, clang-asan, ...) never do --
# GitHub-hosted ubuntu-24.04 apparently ships a modern-enough libstdc++ by
# default, which this doesn't unless it's here explicitly. Same PPA
# ci.yml's own gcc-15 step uses, so this tracks that step's version rather
# than diverging from it.
RUN add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update -q \
    && apt-get install -y --no-install-recommends libstdc++-15-dev \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash runner \
    && usermod -aG sudo runner \
    && echo "runner ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/runner

# GitHub-hosted ubuntu-24.04 runners ship /usr/local/bin writable by the
# runner user (ci.yml's "Install sccache" step untars straight into it,
# with no sudo). Stock ubuntu:24.04 leaves it root:root 755, which makes
# that same step fail with "Permission denied" here.
RUN chown runner:runner /usr/local/bin

USER runner
WORKDIR /home/runner/actions-runner

RUN curl -fsSL -o actions-runner.tar.gz \
        "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-${TARGETARCH}-${RUNNER_VERSION}.tar.gz" \
    && tar xzf actions-runner.tar.gz \
    && rm actions-runner.tar.gz

# Installs libicu/libssl/etc. that the runner's dotnet host needs.
RUN sudo ./bin/installdependencies.sh

COPY --chown=runner:runner entrypoint.sh /home/runner/entrypoint.sh
RUN sudo chmod +x /home/runner/entrypoint.sh

ENTRYPOINT ["/home/runner/entrypoint.sh"]
