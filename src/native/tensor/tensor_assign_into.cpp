/*---
    tensor_assign_into.cpp — v0.14.8: assignInto, the slice-assignment
    write half. `tensor.assignInto(view, values)` writes the values
    over the view's cells (exact shape, cell-for-cell);
    `tensor.assignIntoS(view, scalar)` writes the constant to every
    cell (the region-zeroing spelling). The view's strides route each
    write to the owning record — a slice view writes through to its
    base — and the writeability gate honors the store gate's rule: a
    broadcast axis (stride 0 over dim > 1) folds several coordinates
    onto one cell, so the view is read-only.

    Residency: the view's payload pointer is the owner's buffer under
    both backends (mirror keeps it in host memory, cuda on the
    device) — the CPU walk serves host and mirror views directly; a
    cuda-resident view routes to the stride-aware device kernel with
    the values staged H2D via the pool scratch. ASYNC-GROUP: no cell
    errors (no indices to bound; the gates rode the host checks).
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

#include <cstdint>
#include <string>

using nlang::native::ArgArray;
using nlang::native::ArgDouble;
using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The writeability gate: a broadcast axis (stride 0 over dim > 1)
//folds several coordinates onto one owner cell — the view is
//read-only by the store gate's rule (§4.10.9), and the assignment
//honors it.
void RejectReadOnlyView(NativeHost* host, const NativeArrayView& view) {
    for (int32_t d = 0; d < view.rank; ++d) {
        if (view.strides[d] == 0 && view.dims[d] > 1)
            Raise(host, NEXC_Base,
                "tensor.assignInto: the view is read-only (a broadcast "
                "axis folds several coordinates onto one cell)");
    }
}

//The shape gate: the values' shape must equal the view's (v1 carries
//no broadcasting — numpy's misaligned-shape diagnostic is the honest
//failure; a broadcasting consumer would widen this gate).
void RejectShapeMismatch(NativeHost* host, const NativeArrayView& view,
                         const NativeArrayView& values) {
    if (values.rank != view.rank) {
        Raise(host, NEXC_Base,
              "tensor.assignInto: value rank " + std::to_string(values.rank)
              + " does not match the view rank "
              + std::to_string(view.rank));
    }
    for (int32_t d = 0; d < view.rank; ++d) {
        if (values.dims[d] != view.dims[d])
            Raise(host, NEXC_Base,
                  "tensor.assignInto: value shape "
                  + ShapeText(values) + " does not match the view shape "
                  + ShapeText(view));
    }
}

//The CPU walk: row-major over the view's dims, each cell written at
//data[offset + Σ(strides·coords)] — the read addressing mirrored. The
//values ride the SAME coordinates through their own strides (the
//shapes match per the gate, so one odometer serves both views).
template <typename T>
void AssignIntoCpuWalk(NativeArrayView& view,
                       const NativeArrayView& values) {
    if (view.elemCount == 0)
        return;   //an empty selection writes nothing (the walk's
                  //write-then-advance shape would touch one cell)
    T* cells = static_cast<T*>(view.data);
    const T* valCells = static_cast<const T*>(values.data);
    int64_t coords[nlang::kMaxArrayRank] = {0};
    for (;;) {
        int64_t addr = view.offset;
        int64_t valAddr = values.offset;
        for (int32_t d = 0; d < view.rank; ++d) {
            addr += coords[d] * view.strides[d];
            valAddr += coords[d] * values.strides[d];
        }
        cells[addr] = valCells[valAddr];
        int32_t d = view.rank - 1;
        for (; d >= 0; --d) {
            coords[d] += 1;
            if (coords[d] < view.dims[d])
                break;
            coords[d] = 0;
        }
        if (d < 0)
            return;
    }
}

//The scalar walk: every view cell takes the constant.
template <typename T>
void AssignIntoScalarWalk(NativeArrayView& view, T value) {
    if (view.elemCount == 0)
        return;   //the same write-then-advance shape guard
    T* cells = static_cast<T*>(view.data);
    int64_t coords[nlang::kMaxArrayRank] = {0};
    for (;;) {
        int64_t addr = view.offset;
        for (int32_t d = 0; d < view.rank; ++d)
            addr += coords[d] * view.strides[d];
        cells[addr] = value;
        int32_t d = view.rank - 1;
        for (; d >= 0; --d) {
            coords[d] += 1;
            if (coords[d] < view.dims[d])
                break;
            coords[d] = 0;
        }
        if (d < 0)
            return;
    }
}

struct AssignScalar {
    double sv = 0.0;
    int64_t isv = 0;
};

//The residency route: the slot call per element kind and arm (the
//CPU walk serves host and mirror views; the device slot writes the
//owner's buffer stride-aware).
void RouteAssignInto(const TensorOps& ops, NativeHost* host,
                     NativeArrayView& view, NativeArrayView& values,
                     const AssignScalar& scalar, bool valuesScalar) {
    switch (view.elemKind) {
        case NEK_Int32:
            if (valuesScalar)
                ops.assignIntoSI32(host, view, scalar.isv);
            else
                ops.assignIntoI32(host, view, values);
            break;
        case NEK_Int64:
            if (valuesScalar)
                ops.assignIntoSI64(host, view, scalar.isv);
            else
                ops.assignIntoI64(host, view, values);
            break;
        case NEK_Float32:
            if (valuesScalar)
                ops.assignIntoSF32(host, view, scalar.sv);
            else
                ops.assignIntoF32(host, view, values);
            break;
        default:
            if (valuesScalar)
                ops.assignIntoSF64(host, view, scalar.sv);
            else
                ops.assignIntoF64(host, view, values);
            break;
    }
}

//The shared entry: gates, then the residency route.
void AssignIntoRun(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   bool valuesScalar)
{
    (void)ret;
    NativeArrayView view;
    ArgArray(host, args, 0, &view);
    RejectReadOnlyView(host, view);
    AssignScalar scalar;
    NativeArrayView values;
    std::vector<uint8_t> valStage;
    if (!valuesScalar) {
        ArgArray(host, args, 1, &values);
        if (values.elemKind != view.elemKind)
            Raise(host, NEXC_Base,
                  "tensor.assignInto: element type mismatch");
        RejectShapeMismatch(host, view, values);
        //Metadata staging (the gather C4 pattern): a device-resident
        //values view stages to host before any read.
        values = StageOperandToHost(host, values, &valStage);
    } else {
        switch (view.elemKind) {
            case NEK_Int32: scalar.isv = ArgInt(args, 1); break;
            case NEK_Int64: scalar.isv = ArgLong(args, 1); break;
            case NEK_Float32: scalar.sv = ArgFloat(args, 1); break;
            default: scalar.sv = ArgDouble(args, 1); break;
        }
    }
    const TensorOps& ops = OpsForResidency(view.reserved[0]);
    RouteAssignInto(ops, host, view, values, scalar, valuesScalar);
}

} //namespace

void TensorAssignInto(NativeHost* host, uint8_t* ret, const uint8_t* args,
                      int argc) {
    (void)argc;
    AssignIntoRun(host, ret, args, false);
}

void TensorAssignIntoS(NativeHost* host, uint8_t* ret, const uint8_t* args,
                       int argc) {
    (void)argc;
    AssignIntoRun(host, ret, args, true);
}

//The exported CPU walks (the CPU table's slots call them; the walks
//are file-local here).
void AssignIntoCpuF32(NativeArrayView& view, const NativeArrayView& values) {
    AssignIntoCpuWalk<float>(view, values);
}
void AssignIntoCpuF64(NativeArrayView& view, const NativeArrayView& values) {
    AssignIntoCpuWalk<double>(view, values);
}
void AssignIntoCpuI32(NativeArrayView& view, const NativeArrayView& values) {
    AssignIntoCpuWalk<int32_t>(view, values);
}
void AssignIntoCpuI64(NativeArrayView& view, const NativeArrayView& values) {
    AssignIntoCpuWalk<int64_t>(view, values);
}
void AssignIntoScalarCpuF32(NativeArrayView& view, double sv) {
    AssignIntoScalarWalk<float>(view, static_cast<float>(sv));
}
void AssignIntoScalarCpuF64(NativeArrayView& view, double sv) {
    AssignIntoScalarWalk<double>(view, sv);
}
void AssignIntoScalarCpuI32(NativeArrayView& view, int64_t sv) {
    AssignIntoScalarWalk<int32_t>(view, static_cast<int32_t>(sv));
}
void AssignIntoScalarCpuI64(NativeArrayView& view, int64_t sv) {
    AssignIntoScalarWalk<int64_t>(view, sv);
}

} // namespace tensor
