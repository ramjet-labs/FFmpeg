/*
 * Copyright 2016 Rockchip Electronics S.LSI Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#ifndef __VPU_SIMPLE_MEM_POOL_H__
#define __VPU_SIMPLE_MEM_POOL_H__

#include <stdlib.h>

#define VPU_SIMPLE_MEM_POOL_FUNC_PTR \
	int (*commit_hdl)(void *pool, int hdl, int size); \
	void* (*get_free)(void *pool); \
	int (*inc_used)(void *pool, int mem_hdl); \
	int (*put_used)(void *pool, int mem_hdl); \
	int (*reset)(void *pool); \
	int (*get_unused_num)(void *pool); \
	int reserver1; \
	float version; \
	int reserver2[18];

struct vpu_simple_mem_pool {
	VPU_SIMPLE_MEM_POOL_FUNC_PTR
};

struct vpu_simple_mem_pool* open_vpu_simple_mem_pool(int authenticate_fd);
void close_vpu_simple_mem_pool(struct vpu_simple_mem_pool *pool);

#endif
