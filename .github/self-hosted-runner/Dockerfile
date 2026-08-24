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

ENV DEBIAN_FRONTEND=noninteractive

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
