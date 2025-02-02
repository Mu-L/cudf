# Copyright (c) 2020, NVIDIA CORPORATION.

from libcpp cimport bool
from libcpp.memory cimport unique_ptr
from libcpp.pair cimport pair
from libcpp.string cimport string

from rmm._lib.device_buffer cimport device_buffer

from cudf._lib.cpp.column.column cimport column
from cudf._lib.cpp.column.column_view cimport column_view
from cudf._lib.cpp.table.table cimport table
from cudf._lib.cpp.table.table_view cimport table_view
from cudf._lib.cpp.types cimport bitmask_type, data_type, size_type


cdef extern from "cudf/transform.hpp" namespace "cudf" nogil:
    cdef pair[unique_ptr[device_buffer], size_type] bools_to_mask (
        column_view input
    ) except +

    cdef unique_ptr[column] mask_to_bools (
        bitmask_type* bitmask, size_type begin_bit, size_type end_bit
    ) except +

    cdef pair[unique_ptr[device_buffer], size_type] nans_to_nulls(
        column_view input
    ) except +

    cdef unique_ptr[column] transform(
        column_view input,
        string unary_udf,
        data_type output_type,
        bool is_ptx
    ) except +

    cdef unique_ptr[column] generalized_masked_op(
        const table_view& data_view,
        string udf,
        data_type output_type,
    ) except +

    cdef pair[unique_ptr[table], unique_ptr[column]] encode(
        table_view input
    ) except +

    cdef pair[unique_ptr[column], table_view] one_hot_encode(
        column_view input_column,
        column_view categories
    )
