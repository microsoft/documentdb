/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * include/vector/vector_configs.h
 *
 * Vector index configuration
 *
 *-------------------------------------------------------------------------
 */
#ifndef VECTOR_CONFIGS__H
#define VECTOR_CONFIGS__H

/* Possible iterative scan modes for pre-filtering */
typedef enum VectorIterativeScanMode
{
	VectorIterativeScan_OFF = 0,

	/* Relaxed allows results to be slightly out of order by distance, but provides better recall */
	VectorIterativeScan_RELAXED_ORDER = 1,

	/* Strict ensures results are in the exact order by distance */
	VectorIterativeScan_STRICT_ORDER = 2
} VectorIterativeScanMode;

#endif
