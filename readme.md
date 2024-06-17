## Core
Core is a fork of Apollo cyber, a publish-subscribe system used as middleware for autonomous driving.

## Benchmark
Latency of publish and subscribe messages.

## How to build

1. Run the installation script

```bash
sudo bash scripts/deps/install_fast-rtps.sh
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

1. Build the Publisher and Subscriber
   ```bash
   bazel build //cyber/examples:talker
   bazel build //cyber/examples:listener
   ```

2. Set Environment Variable
   ```bash
   export CYBER_PATH="/workspaces/core/cyber"
   ```

3. Run the Publisher and Subscriber
   - In two separate terminals, run the following commands:
     ```bash
     ./bazel-bin/cyber/examples/listener
     ./bazel-bin/cyber/examples/talker
     ```
   - **Explanation**:
     - In the first terminal, run `./bazel-bin/cyber/examples/listener` to start the subscriber program.
     - In the second terminal, run `./bazel-bin/cyber/examples/talker` to start the publisher program.
     - The publisher (`talker`) will begin publishing messages, and the subscriber (`listener`) will receive and process these messages.
