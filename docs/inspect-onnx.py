"""Prints the graph inputs and outputs of an ONNX file.

Reads the protobuf directly rather than importing onnx, so the model can be
checked on a machine that only has a bare Python install.  Only the handful of
ModelProto/GraphProto/ValueInfoProto fields needed to recover io names and
shapes are decoded; everything else is skipped.
"""

import sys
import struct


def read_varint(buf, pos):
    result = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def iter_fields(buf, start, end):
    """Yields (field_number, wire_type, payload, next_pos)."""
    pos = start
    while pos < end:
        key, pos = read_varint(buf, pos)
        field, wire = key >> 3, key & 7

        if wire == 0:  # varint
            value, pos = read_varint(buf, pos)
            yield field, wire, value
        elif wire == 2:  # length-delimited
            length, pos = read_varint(buf, pos)
            yield field, wire, buf[pos:pos + length]
            pos += length
        elif wire == 5:  # fixed32
            yield field, wire, buf[pos:pos + 4]
            pos += 4
        elif wire == 1:  # fixed64
            yield field, wire, buf[pos:pos + 8]
            pos += 8
        else:
            raise ValueError("unsupported wire type %d" % wire)


ELEM_TYPES = {1: "float32", 2: "uint8", 3: "int8", 6: "int32", 7: "int64", 10: "float16", 11: "double"}


def parse_tensor_shape(buf):
    """TensorShapeProto: repeated Dimension dim = 1."""
    dims = []
    for field, wire, payload in iter_fields(buf, 0, len(buf)):
        if field == 1 and wire == 2:
            # Dimension: dim_value = 1 (int64), dim_param = 2 (string)
            entry = "?"
            for f2, w2, p2 in iter_fields(payload, 0, len(payload)):
                if f2 == 1 and w2 == 0:
                    entry = str(p2)
                elif f2 == 2 and w2 == 2:
                    entry = p2.decode("utf-8", "replace") or "?"
            dims.append(entry)
    return dims


def parse_type(buf):
    """TypeProto: Tensor tensor_type = 1."""
    for field, wire, payload in iter_fields(buf, 0, len(buf)):
        if field == 1 and wire == 2:
            elem, shape = None, []
            for f2, w2, p2 in iter_fields(payload, 0, len(payload)):
                if f2 == 1 and w2 == 0:
                    elem = p2
                elif f2 == 2 and w2 == 2:
                    shape = parse_tensor_shape(p2)
            return ELEM_TYPES.get(elem, "type%s" % elem), shape
    return "?", []


def parse_value_info(buf):
    """ValueInfoProto: name = 1, type = 2."""
    name, dtype, shape = "?", "?", []
    for field, wire, payload in iter_fields(buf, 0, len(buf)):
        if field == 1 and wire == 2:
            name = payload.decode("utf-8", "replace")
        elif field == 2 and wire == 2:
            dtype, shape = parse_type(payload)
    return name, dtype, shape


def main(path):
    with open(path, "rb") as handle:
        data = handle.read()

    graph = None
    for field, wire, payload in iter_fields(data, 0, len(data)):
        if field == 7 and wire == 2:  # ModelProto.graph
            graph = payload
            break

    if graph is None:
        print("no graph found")
        return 1

    inputs, outputs, initializers = [], [], set()
    for field, wire, payload in iter_fields(graph, 0, len(graph)):
        if field == 11 and wire == 2:  # input
            inputs.append(parse_value_info(payload))
        elif field == 12 and wire == 2:  # output
            outputs.append(parse_value_info(payload))
        elif field == 5 and wire == 2:  # initializer (weights)
            for f2, w2, p2 in iter_fields(payload, 0, len(payload)):
                if f2 == 8 and w2 == 2:
                    initializers.add(p2.decode("utf-8", "replace"))

    # Weights show up as graph inputs in some exports; they are not real
    # inputs and would badly skew the count.
    real_inputs = [i for i in inputs if i[0] not in initializers]

    print("INPUTS (%d):" % len(real_inputs))
    for name, dtype, shape in real_inputs:
        print("  %-20s %-8s [%s]" % (name, dtype, ", ".join(shape)))

    print("OUTPUTS (%d):" % len(outputs))
    for name, dtype, shape in outputs:
        print("  %-20s %-8s [%s]" % (name, dtype, ", ".join(shape)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "modnet.onnx"))
