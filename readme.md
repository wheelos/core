## Core
Core is a fork of Apollo cyber, a publish-subscribe system used as middleware for autonomous driving.

## Benchmark
Latency of publish and subscribe messages.

## How to build

1. Run the installation script

```bash
sudo bash scripts/deploy.sh
```

2. Set environment variables

```bash
export CPLUS_INCLUDE_PATH="/usr/local/fast-rtps/include"
```

3. Run the build script

```bash
bash scripts/build.sh
```

## Example

1. Set Environment Variable
   ```bash
   source scripts/env/setup.bash
   ```

2. Run the Publisher and Subscriber
   - In the first terminal, run `./bazel-bin/cyber/examples/listener` to start the subscriber program.
   - In the second terminal, run `./bazel-bin/cyber/examples/talker` to start the publisher program.

3. Check the log in `data/log`
