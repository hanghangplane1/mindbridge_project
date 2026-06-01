#!/bin/bash
# Wrapper script to run eBPF monitor with sudo
# Uses printf to avoid stdin interference with the child process's stdout

if [ -n "$MINDBRIDGE_EBPF_PID" ]; then
    printf ' \n' | sudo -S -E MINDBRIDGE_EBPF_PID="$MINDBRIDGE_EBPF_PID" "$@"
else
    printf ' \n' | sudo -S "$@"
fi
