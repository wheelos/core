#!/usr/bin/env python3
"""Workers imported only by the freshly installed wheel's Python interpreter."""

import argparse
import json
import signal
import struct
import time
from pathlib import Path

import pycyber
from google.protobuf.descriptor_pb2 import FileDescriptorProto
from google.protobuf.wrappers_pb2 import BytesValue
from pycyber import cyber, record
from pycyber.proto import record_pb2

COUNT = 40


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def payload(token, sequence):
    return struct.pack(">I", sequence) + b"\0wheelos-e2e\0" + token.encode() + b"\0\xff"


def validate(value, token):
    require(len(value) >= 4, "truncated payload")
    sequence = struct.unpack(">I", value[:4])[0]
    require(value == payload(token, sequence), "binary payload corruption")
    require(0 <= sequence <= COUNT, "sequence outside published range")
    return sequence


def wait_until(predicate, timeout=25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("worker deadline exceeded")


def inspect_record(args):
    reader = record.RecordReader(args.record)
    require(reader.get_channellist() == [args.channel], "unexpected record channels")
    require(
        reader.get_messagetype(args.channel) == BytesValue.DESCRIPTOR.full_name,
        "record message type mismatch",
    )
    require(bool(reader.get_protodesc(args.channel)), "missing record descriptor")
    header = record_pb2.Header()
    header.ParseFromString(reader.get_headerstring())
    require(header.is_complete, "incomplete native recording")
    sequences = []
    timestamps = []
    for channel, raw, datatype, timestamp in reader.read_messages():
        require(channel == args.channel, "record channel mismatch")
        require(datatype == BytesValue.DESCRIPTOR.full_name, "record datatype mismatch")
        sequence = validate(BytesValue.FromString(raw).value, args.token)
        if sequence:
            sequences.append(sequence)
            timestamps.append(timestamp)
    require(sequences == list(range(1, COUNT + 1)), repr(sequences))
    require(timestamps == sorted(timestamps), "record timestamps are not ordered")
    require(all(timestamp > 0 for timestamp in timestamps), "zero record timestamp")
    require(
        reader.get_messagenumber(args.channel) >= COUNT,
        "record index count does not include published data",
    )
    if args.role == "wheel-record":
        require(
            timestamps == [sequence * 1000000 for sequence in sequences],
            "wheel record timestamp round trip",
        )
    Path(args.result).write_text(
        json.dumps(
            {"sequences": sequences, "message_type": BytesValue.DESCRIPTOR.full_name}
        )
    )
    reader.close()


def run(args):
    require(
        Path(pycyber.__file__).resolve().is_relative_to(Path(args.venv).resolve()),
        "pycyber was not imported from the fresh venv",
    )
    if args.role in ("inspect-record", "wheel-record"):
        if args.role == "wheel-record":
            descriptor = FileDescriptorProto()
            BytesValue.DESCRIPTOR.file.CopyToProto(descriptor)
            with record.RecordWriter() as writer:
                require(writer.open(args.record), "record open failed")
                require(
                    writer.write_channel(
                        args.channel,
                        BytesValue.DESCRIPTOR.full_name,
                        descriptor.SerializeToString(),
                    ),
                    "write_channel failed",
                )
                for sequence in range(1, COUNT + 1):
                    message = BytesValue(value=payload(args.token, sequence))
                    require(
                        writer.write_message(
                            args.channel,
                            message.SerializeToString(),
                            sequence * 1000000,
                        ),
                        "write_message failed",
                    )
                require(writer.get_messagenumber(args.channel) == COUNT, "write count")
        inspect_record(args)
        return
    cyber.init("installed_e2e_" + args.token + "_" + args.role)
    node = cyber.Node("e2e_" + args.token + "_" + args.role)
    stopping = []
    signal.signal(signal.SIGINT, lambda *_: stopping.append(True))
    signal.signal(signal.SIGTERM, lambda *_: stopping.append(True))
    try:
        if args.role == "publisher":
            writer = node.create_writer(args.channel, BytesValue, 128)
            require(writer.data_type == BytesValue.DESCRIPTOR.full_name, "writer type")
            Path(args.ready).touch()
            deadline = time.monotonic() + 25
            while not Path(args.gate).exists():
                require(not stopping and time.monotonic() < deadline, "publisher gate")
                require(writer.write(BytesValue(value=payload(args.token, 0))), "probe")
                time.sleep(0.05)
            for sequence in range(1, COUNT + 1):
                require(
                    writer.write(BytesValue(value=payload(args.token, sequence))),
                    "write returned false",
                )
                time.sleep(0.05)
            time.sleep(0.3)
            Path(args.result).write_text(json.dumps({"published": COUNT}))
        elif args.role == "subscriber":
            received, failures = [], []

            def callback(message):
                try:
                    sequence = validate(message.value, args.token)
                    if sequence == 0:
                        Path(args.connected).touch()
                    else:
                        received.append(sequence)
                except Exception as error:
                    failures.append(str(error))

            reader = node.create_reader(args.channel, BytesValue, callback)
            require(reader.name == args.channel, "reader channel")
            Path(args.ready).touch()
            wait_until(lambda: failures or len(received) >= COUNT or stopping)
            require(not failures, repr(failures))
            require(received == list(range(1, COUNT + 1)), repr(received))
            Path(args.result).write_text(json.dumps({"sequences": received}))
        elif args.role == "service":
            service = node.create_service(
                args.channel,
                BytesValue,
                BytesValue,
                lambda request: BytesValue(value=request.value + b"\0response"),
            )
            require(service.name == args.channel, "service name")
            Path(args.ready).touch()
            wait_until(lambda: stopping, timeout=45)
        elif args.role == "client":
            client = node.create_client(args.channel, BytesValue, BytesValue)
            replies = []
            for sequence in range(1, 6):
                request = BytesValue(value=payload(args.token, sequence))
                deadline = time.monotonic() + 12
                response = None
                while response is None and time.monotonic() < deadline:
                    response = client.send_request(request)
                    if response is None:
                        time.sleep(0.1)
                require(response is not None, "RPC did not discover server")
                require(
                    response.value == request.value + b"\0response",
                    "RPC response bytes mismatch",
                )
                replies.append(sequence)
            Path(args.result).write_text(json.dumps({"replies": replies}))
    finally:
        node.close()
        cyber.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--role",
        required=True,
        choices=[
            "publisher",
            "subscriber",
            "service",
            "client",
            "inspect-record",
            "wheel-record",
        ],
    )
    for option in ("token", "channel", "venv", "ready", "connected", "gate", "result"):
        parser.add_argument("--" + option, required=True)
    parser.add_argument("--record")
    run(parser.parse_args())
