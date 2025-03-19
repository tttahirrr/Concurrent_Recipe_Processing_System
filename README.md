README.md


# Concurrent Recipe Processing System
# Overview
This project implements a concurrent task scheduler for executing interdependent recipes, simulating a kitchen environment. Recipes are parsed from a cookbook file, dependencies are resolved, and tasks are executed using process-based parallelism with inter-process communication.

# Key Features
Dynamic task scheduling based on recipe dependencies (DAG).
Concurrent processing using fork() and POSIX signals (SIGCHLD) for managing task lifecycles.
Supports input/output redirection for tasks.
Executes commands in util/ directory or falls back to system commands.
Handles execution failures, timeouts, and resource cleanup.

# Build Instructions
Requirements:
gcc, make
criterion for unit tests
pthread
Build Commands:
make             # Build release version
make debug       # Build with debugging and color logs
make clean       # Remove build artifacts

# Executables:
Main binary: bin/cook
Test binary: bin/cook_tests

# Usage
Run the main program:
bin/cook < cookbook.txt

Run tests:
bin/cook_tests


# Implementation Highlights
Task execution pipeline: Each TASK consists of multiple STEPs, executed in isolated child processes.
WorkQueue: Manages ready-to-run recipes; dynamically updated as dependencies are resolved.
Signals: Uses SIGCHLD handlers to detect task completion and trigger dependent tasks.
Resource management: Ensures file descriptors and memory are properly handled in all execution paths.