#include "voxel.h"
#include "utility/shader.h"
#include "utility/texture.h"
#include "globals.h"
#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <memory.h>
#include <string.h>
#include "math/all.h"

//--------------------------------------------------------------------------------------------------------------------------------//
//GLOBAL STATE:

GLuint lightingRequestBuffer = 0;
GLuint materialBuffer = 0;
GLprogram lightingProgram = 0;
GLprogram finalProgram = 0;

DNmaterial* dnMaterials = 0;

unsigned int maxLightingRequests = 1024;

const size_t VOXEL_DATA_SIZE = sizeof(DNvoxel) * 8 * 8 * 8;
const size_t LIGHTING_DATA_SIZE = sizeof(DNvec4) * 8 * 8 * 8;
const int WORKGROUP_SIZE = 16;

//--------------------------------------------------------------------------------------------------------------------------------//
//INITIALIZATION:

static bool _DN_gen_shader_storage_buffer(unsigned int* dest, size_t size);
static void _DN_clear_chunk(DNmap* map, unsigned int index);

bool DN_init()
{	
	//allocate memory:
	//---------------------------------
	dnMaterials = DN_MALLOC(sizeof(DNmaterial) * DN_MAX_VOXEL_MATERIALS);
	if(!dnMaterials)
	{
		DN_ERROR_LOG("ERROR - FAILED TO ALLOCATE MEMORY FOR VOXEL MATERIALS\n");
		return false;
	}

	//generate gl buffers:
	//---------------------------------
	if(!_DN_gen_shader_storage_buffer(&materialBuffer, sizeof(DNmaterial) * DN_MAX_VOXEL_MATERIALS))
	{
		DN_ERROR_LOG("ERROR - FAILED TO GENERATE VOXEL MATERIAL BUFFER\n");
		return false;
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, materialBuffer);

	if(!_DN_gen_shader_storage_buffer(&lightingRequestBuffer, sizeof(DNuvec4) * maxLightingRequests))
	{
		DN_ERROR_LOG("ERROR - FAILED TO GENERATE VOXEL LIGHTING REQUEST BUFFER\n");
		return false;
	}
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, lightingRequestBuffer);

	//load shaders:
	//---------------------------------
	int lighting = 0;//DN_compute_program_load("shaders/voxelLighting.comp", "shaders/voxelShared.comp");
	int final    = DN_compute_program_load("shaders/voxelFinal.comp"   , "shaders/voxelShared.comp");

	if(lighting < 0 || final < 0)
	{
		DN_ERROR_LOG("ERROR - FAILED TO COMPILE 1 OR MORE VOXEL SHADERS\n");
		return false;
	}

	lightingProgram = lighting;
	finalProgram = final;

	//return:
	//---------------------------------
	return true;
}

void DN_quit()
{
	DN_program_free(lightingProgram);
	DN_program_free(finalProgram);

	glDeleteBuffers(1, &materialBuffer);
	glDeleteBuffers(1, &lightingRequestBuffer);

	DN_FREE(dnMaterials);
}

DNmap* DN_create_map(DNuvec3 mapSize, DNuvec2 textureSize, bool streamable, unsigned int minChunks)
{
	//allocate structure:
	//---------------------------------
	DNmap* map = DN_MALLOC(sizeof(DNmap));

	//generate texture:
	//---------------------------------
	glGenTextures(1, &map->glTextureID);
	glBindTexture(GL_TEXTURE_2D, map->glTextureID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, textureSize.x, textureSize.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &map->glPositionTextureID);
	glBindTexture(GL_TEXTURE_2D, map->glPositionTextureID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, textureSize.x, textureSize.y, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	//generate buffers:
	//---------------------------------
	if(!_DN_gen_shader_storage_buffer(&map->glMapBufferID, sizeof(DNchunkHandle) * mapSize.x * mapSize.y * mapSize.x))
	{
		DN_ERROR_LOG("ERROR - FAILED TO GENERATE GPU BUFFER FOR MAP\n");
		return NULL;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glMapBufferID);
	glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED, GL_UNSIGNED_INT, NULL);

	size_t numChunks;
	if(streamable)
		numChunks = min(mapSize.x * mapSize.y * mapSize.z, minChunks);
	else
		numChunks = min(mapSize.x * mapSize.y * mapSize.z, 64); //set 64 as the minimum to allow for blank space to be ignored

	if(!_DN_gen_shader_storage_buffer(&map->glChunkBufferID, sizeof(DNchunk) * numChunks))
	{
		DN_ERROR_LOG("ERROR - FAILED TO GENERATE GPU BUFFER FOR CHUNKS\n");
		return NULL;
	}
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glChunkBufferID);
	glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED, GL_UNSIGNED_INT, NULL);

	//allocate CPU memory:
	//---------------------------------
	map->map = DN_MALLOC(sizeof(DNchunkHandle) * mapSize.x * mapSize.y * mapSize.z);
	if(!map->map)
	{
		DN_ERROR_LOG("ERROR - FAILED TO ALLOCATE CPU MEMORY FOR MAP\n");
		return NULL;
	}

	//set all map tiles to empty:
	for(int i = 0; i < mapSize.x * mapSize.y * mapSize.z; i++)
		map->map[i].flag = map->map[i].visible = 0;

	map->chunks = DN_MALLOC(sizeof(DNchunk) * numChunks);
	if(!map->chunks)
	{
		DN_ERROR_LOG("ERROR - FAILED TO ALLOCATE CPU MEMORY FOR CHUNKS\n");
		return NULL;
	}

	//set all voxels to empty:
	for(int i = 0; i < numChunks; i++)
		_DN_clear_chunk(map, i);

	map->lightingRequests = DN_MALLOC(sizeof(DNuvec4) * numChunks);
	if(!map->lightingRequests)
	{
		DN_ERROR_LOG("ERROR - FAILED TO ALLOCATE CPU MEMORY FOR LIGHTING REQUESTS\n");
		return NULL;
	}

	if(streamable)
	{
		map->gpuChunkLayout = DN_MALLOC(sizeof(DNivec3) * numChunks);
		if(!map->gpuChunkLayout)
		{
			DN_ERROR_LOG("ERROR - FAILED TO ALLOCATE CPU MEMORY FOR GPU CHUNK LAYOUT\n");
			return NULL;
		}

		for(int i = 0; i < numChunks; i++)
			map->gpuChunkLayout[i].x = -1;
	}

	//set data parameters:
	//---------------------------------
	map->mapSize = mapSize;
	map->textureSize = textureSize;
	map->chunkCap = numChunks;
	map->chunkCapGPU = numChunks;
	map->nextChunk = 0;
	map->numLightingRequest = 0;
	map->lightingRequestCap = numChunks;
	map->streamable = streamable;

	//set default camera and lighting parameters:
	//---------------------------------
	map->camPos = (DNvec3){0.0f, 0.0f, 0.0f};
	map->camOrient = (DNvec3){0.0f, 0.0f, 0.0f};
	map->camFOV = 0.8f;
	map->camViewMode = 0;

	map->sunDir = (DNvec3){1.0f, 1.0f, 1.0f};
	map->sunStrength = (DNvec3){0.6f, 0.6f, 0.6f};
	map->ambientLightStrength = 0.01f; //TODO: MAKE THIS A VEC3
	map->diffuseBounceLimit = 5;
	map->specBounceLimit = 2;
	map->shadowSoftness = 10.0f;

	return map;
}

void DN_delete_map(DNmap* map)
{
	glDeleteTextures(1, &map->glTextureID);
	glDeleteBuffers(1, &map->glMapBufferID);
	glDeleteBuffers(1, &map->glChunkBufferID);

	DN_FREE(map->map);
	DN_FREE(map->chunks);
	DN_FREE(map->lightingRequests);
	if(map->streamable)
		DN_FREE(map->gpuChunkLayout);
	DN_FREE(map);
}

//--------------------------------------------------------------------------------------------------------------------------------//
//MEMORY:

unsigned int DN_add_chunk(DNmap* map, DNivec3 pos)
{
	if(!DN_in_map_bounds(map, pos))
	{
		DN_ERROR_LOG("ERROR - OUT OF BOUNDS POSITION TO ADD\n");
		return 0;
	}

	//search for empty chunk:
	int i = map->nextChunk;

	do
	{
		if(!map->chunks[i].used) //if an empty chunk is found, use that one:
		{
			int mapIndex = DN_FLATTEN_INDEX(pos, map->mapSize);
			map->map[mapIndex].index = i;
			map->map[mapIndex].flag = 1;

			map->chunks[i].pos = (DNivec3){pos.x, pos.y, pos.z};
			map->chunks[i].used = true;
			map->nextChunk = (i == map->chunkCap - 1) ? (0) : (i + 1);

			return i;
		}

		//jump to beginning if the end is reached:
		i++;
		if(i >= map->chunkCap)
			i = 0;

	} while(i != map->nextChunk);

	//if no empty chunk is found, increase capacity:
	size_t newCap = min(map->chunkCap * 2, map->mapSize.x * map->mapSize.y * map->mapSize.z);
	DN_ERROR_LOG("NOTE - RESIZING CHUNK MEMORY TO ALLOW FOR %zi CHUNKS\n", newCap);

	i = map->chunkCap;
	if(!DN_set_max_chunks(map, newCap))
		return 0;

	//clear new chunks:
	for(int j = i; j < map->chunkCap; j++)
		_DN_clear_chunk(map, j);

	int mapIndex = DN_FLATTEN_INDEX(pos, map->mapSize);
	map->map[mapIndex].index = i;
	map->map[mapIndex].flag = 1;

	map->chunks[i].pos = (DNivec3){pos.x, pos.y, pos.z};
	map->chunks[i].used = true;
	map->nextChunk = (i == map->chunkCap - 1) ? (0) : (i + 1);

	return i;
}

void DN_remove_chunk(DNmap* map, DNivec3 pos)
{
	if(!DN_in_map_bounds(map, pos))
	{
		DN_ERROR_LOG("ERROR - OUT OF BOUNDS POSITION TO REMOVE\n");
		return;
	}

	int mapIndex = DN_FLATTEN_INDEX(pos, map->mapSize);
	map->map[mapIndex].flag = 0;
	map->nextChunk = map->map[mapIndex].index;
	_DN_clear_chunk(map, map->map[mapIndex].index);
}

static void _DN_stream_voxel_chunk(DNmap* map, DNivec3 pos, DNchunkHandle* voxelMapGPU, unsigned int mapIndex)
{
	//variables that belong to the oldest chunk:
	int maxTime = 0;
	int maxTimeIndex = -1;
	int maxTimeMapIndex = -1;

	//loop through every chunk to look for oldest:
	for(int i = 0; i < map->chunkCapGPU; i++)
	{
		DNivec3 currentPos = map->gpuChunkLayout[i];
		unsigned int currentIndex = DN_FLATTEN_INDEX(currentPos, map->mapSize);

		//if chunk does not belong to any active map tile, instantly use it
		if(!DN_in_map_bounds(map, currentPos))
		{
			maxTime = 2;
			maxTimeIndex = i;
			maxTimeMapIndex = -1;
			break;
		}
		else if(voxelMapGPU[currentIndex].lastUsed > maxTime) //else, check if it is older than the current oldest
		{
			maxTime = voxelMapGPU[currentIndex].lastUsed;
			maxTimeIndex = i;
			maxTimeMapIndex = currentIndex;
		}
	}

	if(maxTime <= 1) //if the oldest chunk is currently in use, double the buffer size
	{
		size_t newCap = min(map->chunkCap, map->chunkCapGPU * 2);
		DN_ERROR_LOG("NOTE - MAXIMUM GPU CHUNK LIMIT REACHED... RESIZING TO %zi CHUNKS\n", newCap);

		//create a temporary buffer to store the old chunk data (opengl doesnt have a "realloc" function)
		void* oldChunkData = DN_MALLOC(sizeof(DNchunk) * map->chunkCapGPU);
		if(!oldChunkData)
		{
			DN_ERROR_LOG("ERROR - FAILED TO ALLOCATE MEMORY FOR TEMPORARY CHUNK STORAGE\n");
			return;
		}

		//get old data:
		glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(DNchunk) * map->chunkCapGPU, oldChunkData);

		//resize buffer:
		if(!DN_set_max_chunks_gpu(map, newCap))
		{
			DN_FREE(oldChunkData);
			return;
		}

		//send back old data:
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(DNchunk) * map->chunkCapGPU, oldChunkData);

		//resize chunk layout memory:
		DNivec3* newChunkLayout = DN_REALLOC(map->gpuChunkLayout, sizeof(DNivec3) * newCap);
		if(!newChunkLayout)
		{
			DN_ERROR_LOG("ERROR - UNABLE TO ALLOCATE MEMORY FOR NEW CHUNK LAYOUT\n");
			DN_FREE(oldChunkData);
			return;
		}
		map->gpuChunkLayout = newChunkLayout;

		//clear new chunk layout memory:
		for(int i = map->chunkCapGPU; i < newCap; i++)
			map->gpuChunkLayout[i].x = -1;

		//set max time variables:
		maxTimeMapIndex = -1;
		maxTimeIndex = map->chunkCapGPU;

		map->chunkCapGPU = newCap;
		DN_FREE(oldChunkData);
	}

	if(maxTimeMapIndex >= 0) //if the chunk was previously loaded, set its flag to unloaded and store old data (to maintain semi-baked lighting)
	{
		map->map[maxTimeMapIndex].flag = map->map[maxTimeMapIndex].flag != 0 ? 1 : 0;
		voxelMapGPU[maxTimeMapIndex].flag = map->map[maxTimeMapIndex].flag;
		glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, (maxTimeIndex + 1) * sizeof(DNchunk) - LIGHTING_DATA_SIZE, LIGHTING_DATA_SIZE, map->chunks[map->map[maxTimeMapIndex].index].indirectLight);
	}

	voxelMapGPU[mapIndex].flag = 2; //set the new tile's flag to loaded
	map->map[mapIndex].flag = 2;
	voxelMapGPU[mapIndex].lastUsed = 0;
	voxelMapGPU[mapIndex].index = maxTimeIndex;

	//load in new data:
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, maxTimeIndex * sizeof(DNchunk), sizeof(DNchunk), &map->chunks[map->map[mapIndex].index]);
	map->chunks[map->map[mapIndex].index].updated = false;
	map->gpuChunkLayout[maxTimeIndex] = pos;
}

static void _DN_sync_gpu_streamable(DNmap* map, DNmemOp op, DNchunkRequests requests)
{
	//map the buffer:
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glMapBufferID);
	DNchunkHandle* voxelMapGPU = (DNchunkHandle*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_WRITE);

	//bind the chunk buffer so it can be updated:
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glChunkBufferID);

	if(op != DN_WRITE && requests != DN_REQUEST_NONE)
		map->numLightingRequest = 0;

	//loop through every map tile:
	for(int z = 0; z < map->mapSize.z; z++)
	for(int y = 0; y < map->mapSize.y; y++)
	for(int x = 0; x < map->mapSize.x; x++)
	{
		DNivec3 pos = {x, y, z};
		int mapIndex = DN_FLATTEN_INDEX(pos, map->mapSize);    //the map index for the cpu-side memory
		DNchunkHandle cell = voxelMapGPU[mapIndex];

		if(op != DN_READ)
		{
			//if a chunk was added to the cpu map, request it to be added to the gpu map:
			if(map->map[mapIndex].flag != 0 && cell.flag == 0)
				voxelMapGPU[mapIndex].flag = 1;
			else if(map->map[mapIndex].flag == 0 && cell.flag != 0)
			{
				voxelMapGPU[mapIndex].flag = 0;
				map->gpuChunkLayout[cell.index].x = -1;
			}

			if(map->map[mapIndex].flag != 0 && map->chunks[map->map[mapIndex].index].updated)
			{
				if(cell.flag == 2)
					map->gpuChunkLayout[cell.index].x = -1;
				voxelMapGPU[mapIndex].flag = cell.flag >= 2 ? 3 : 1;
				map->chunks[map->map[mapIndex].index].updated = false;
			}

			//if flag = 3 (requested), try to load a new chunk:
			if(cell.flag == 3 && map->map[mapIndex].flag != 0)
				_DN_stream_voxel_chunk(map, pos, voxelMapGPU, mapIndex);
		}

		if(op != DN_WRITE)
		{
			//if chunk is loaded and visible, add to lighting request buffer:
			if(requests != DN_REQUEST_NONE && cell.flag >= 1 && (requests == DN_REQUEST_LOADED || cell.visible == 1))
			{
				if(map->numLightingRequest >= map->lightingRequestCap)
				{
					size_t newCap = min(map->mapSize.x * map->mapSize.y * map->mapSize.z, map->lightingRequestCap * 2);
					DN_ERROR_LOG("NOTE - RESIZING LIGHTING REQUEST MEMORY TO ALLOW FOR %zi REQUESTS\n", newCap);
					if(!DN_set_max_lighting_requests(map, newCap))
						break;
				}

				map->lightingRequests[map->numLightingRequest++].x = cell.index;
			}

 			//set the "visible" flag to 0:
			voxelMapGPU[mapIndex].visible = 0;

			//increase the "time last used" flag:
			voxelMapGPU[mapIndex].lastUsed++;
		}
	}

	//unmap:
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glMapBufferID);
	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
}

static void _DN_sync_gpu_nonstreamable(DNmap* map, DNmemOp op, DNchunkRequests requests)
{
	if(op != DN_READ)
	{
		//send chunks:
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glChunkBufferID);

		if(map->chunkCap == map->chunkCapGPU) //if chunk buffer was not resized, only send the updated chunks:
		{
			for(int i = 0; i < map->chunkCap; i++)
			{
				DNchunk chunk = map->chunks[i];
				if(chunk.used && chunk.updated)
				{
					glBufferSubData(GL_SHADER_STORAGE_BUFFER, i * sizeof(DNchunk), VOXEL_DATA_SIZE, &chunk);
					map->chunks[i].updated = 0;
					map->map[DN_FLATTEN_INDEX(chunk.pos, map->mapSize)].flag = 2;
				}
			}
		}
		else //resize and reupload buffer if a new size is needed:
		{
			glBufferData(GL_SHADER_STORAGE_BUFFER, map->chunkCap * sizeof(DNchunk), map->chunks, GL_DYNAMIC_DRAW);
			map->chunkCapGPU = map->chunkCap;

			for(int i = 0; i < map->chunkCap; i++)
				if(map->chunks[i].used)
				{
					map->chunks[i].updated = 0;
					map->map[DN_FLATTEN_INDEX(map->chunks[i].pos, map->mapSize)].flag = 2;
				}
		}
	}

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glMapBufferID);
	DNchunkHandle* gpuMap = glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_WRITE);

	if(op != DN_WRITE && requests != DN_REQUEST_NONE)
		map->numLightingRequest = 0;

	unsigned int maxIndex = map->mapSize.x * map->mapSize.y * map->mapSize.z;
	for(int i = 0; i < maxIndex; i++)
	{
		DNchunkHandle cell = gpuMap[i]; //if we are going to upload a new map, use data from that new map instead of old data

		if(op != DN_READ)
		{
			if(cell.flag != map->map[i].flag)
				gpuMap[i].flag = cell.flag = map->map[i].flag;

			if(cell.index != map->map[i].index)
				gpuMap[i].index = cell.index = map->map[i].index;
		}

		if(op != DN_WRITE)
		{
			if(requests != DN_REQUEST_NONE && cell.flag >= 1 && (requests == DN_REQUEST_LOADED || cell.visible == 1))
			{
				if(map->numLightingRequest >= map->lightingRequestCap)
				{
					size_t newCap = min(maxIndex, map->lightingRequestCap * 2);
					DN_ERROR_LOG("NOTE - RESIZING LIGHTING REQUEST MEMORY TO ALLOW FOR %zi REQUESTS\n", newCap);
					if(!DN_set_max_lighting_requests(map, newCap))
						break;
				}

				map->lightingRequests[map->numLightingRequest++].x = cell.index;
			}

			gpuMap[i].visible = 0;
		}
	}

	glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
}

void DN_sync_gpu(DNmap* map, DNmemOp op, DNchunkRequests requests)
{
	if(map->streamable)
		_DN_sync_gpu_streamable(map, op, requests);
	else
		_DN_sync_gpu_nonstreamable(map, op, requests);
	glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
}

//--------------------------------------------------------------------------------------------------------------------------------//
//UPDATING/DRAWING:

void DN_draw(DNmap* map)
{
	float aspectRatio = (float)map->textureSize.y / (float)map->textureSize.x;
	DNmat3 rotate = DN_mat4_to_mat3(DN_mat4_rotate_euler(DN_MAT4_IDENTITY, map->camOrient));
	DNvec3 camFront = DN_mat3_mult_vec3(rotate, (DNvec3){ 0.0f, 0.0f, map->camFOV });
	DNvec3 camPlaneU;
	DNvec3 camPlaneV;

	if(aspectRatio < 1.0f)
	{
		camPlaneU = DN_mat3_mult_vec3(rotate, (DNvec3){-1.0f, 0.0f, 0.0f});
		camPlaneV = DN_mat3_mult_vec3(rotate, (DNvec3){ 0.0f, 1.0f * aspectRatio, 0.0f});
	}
	else
	{
		camPlaneU = DN_mat3_mult_vec3(rotate, (DNvec3){-1.0f / aspectRatio, 0.0f, 0.0f});
		camPlaneV = DN_mat3_mult_vec3(rotate, (DNvec3){ 0.0f, 1.0f, 0.0f});
	}

	glUseProgram(finalProgram);

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, map->glChunkBufferID);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, map->glMapBufferID);
	glBindImageTexture(0, map->glTextureID, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
	glBindImageTexture(1, map->glPositionTextureID, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32UI);

	DN_program_uniform_vec3 (finalProgram, "camPos", map->camPos);
	DN_program_uniform_vec3 (finalProgram, "camDir", camFront);
	DN_program_uniform_vec3 (finalProgram, "camPlaneU", camPlaneU);
	DN_program_uniform_vec3 (finalProgram, "camPlaneV", camPlaneV);
	DN_program_uniform_vec3 (finalProgram, "sunStrength", map->sunStrength);
	DN_program_uniform_vec3 (finalProgram, "sunDir", DN_vec3_normalize(map->sunDir));
	DN_program_uniform_uint (finalProgram, "viewMode", map->camViewMode);
	DN_program_uniform_float(finalProgram, "ambientStrength", map->ambientLightStrength);
	glUniform3uiv(glGetUniformLocation(finalProgram, "mapSize"), 1, (GLuint*)&map->mapSize);

	glBindImageTexture(0, map->glTextureID, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
	glDispatchCompute(map->textureSize.x / WORKGROUP_SIZE, map->textureSize.y / WORKGROUP_SIZE, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
}

void DN_update_lighting(DNmap* map, unsigned int offset, unsigned int num, float time)
{
	glUseProgram(lightingProgram);

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, map->glChunkBufferID);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, map->glMapBufferID);
	num = num == 0 ? map->numLightingRequest : num;
	if(offset + num > map->lightingRequestCap)
		num = map->lightingRequestCap - offset;

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightingRequestBuffer);

	if(num > maxLightingRequests)
	{
		size_t newSize = maxLightingRequests;
		while(newSize < num)
			newSize *= 2;

		DN_ERROR_LOG("NOTE - RESIZING LIGHTING REQUESTS BUFFER TO ACCOMODATE %zi REQUESTS\n", newSize);
		glBufferData(GL_SHADER_STORAGE_BUFFER, newSize * sizeof(DNuvec4), NULL, GL_DYNAMIC_DRAW);
		if(glGetError() == GL_OUT_OF_MEMORY)
		{
			DN_ERROR_LOG("ERROR - FAILED TO RESIZE LIGHTING REQUESTS BUFFER\n");
			return;
		}
		maxLightingRequests = newSize;
	}

	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, num * sizeof(DNuvec4), &map->lightingRequests[offset]);

	DN_program_uniform_vec3(lightingProgram, "camPos", map->camPos);
	DN_program_uniform_float(lightingProgram, "time", time);
	DN_program_uniform_uint(lightingProgram, "diffuseBounceLimit", map->diffuseBounceLimit);
	DN_program_uniform_uint(lightingProgram, "specularBounceLimit", map->specBounceLimit);
	DN_program_uniform_vec3(lightingProgram, "sunDir", DN_vec3_normalize(map->sunDir));
	DN_program_uniform_vec3(lightingProgram, "sunStrength", map->sunStrength);
	DN_program_uniform_float(lightingProgram, "shadowSoftness", map->shadowSoftness);
	DN_program_uniform_float(lightingProgram, "ambientStrength", map->ambientLightStrength);
	glUniform3uiv(glGetUniformLocation(lightingProgram, "mapSize"), 1, (GLuint*)&map->mapSize);

	glDispatchCompute(num, 1, 1);
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

//--------------------------------------------------------------------------------------------------------------------------------//
//MAP SETTINGS:

bool DN_set_texture_size(DNmap* map, DNuvec2 size)
{
	size.x += WORKGROUP_SIZE - size.x % WORKGROUP_SIZE;
	size.y += WORKGROUP_SIZE - size.y % WORKGROUP_SIZE;

	glBindTexture(GL_TEXTURE_2D, map->glTextureID);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	if(glGetError() == GL_OUT_OF_MEMORY)
	{
		DN_ERROR_LOG("ERROR - FAILED TO RESIZE FINAL TEXTURE\n");
		return false;
	}

	map->textureSize = size;
	return true;
}

bool DN_set_map_size(DNmap* map, DNuvec3 size)
{
	//allocate new map:
	DNchunkHandle* newMap = DN_MALLOC(sizeof(DNchunkHandle) * size.x * size.y * size.z);
	if(!newMap)
	{
		DN_ERROR_LOG("ERROR - FAILED TO REALLOCATE VOXEL MAP\n");
		return false;
	}

	//loop over and copy map data:
	for(int z = 0; z < size.z; z++)
		for(int y = 0; y < size.y; y++)
			for(int x = 0; x < size.x; x++)
			{
				DNivec3 pos = {x, y, z};

				int oldIndex = DN_FLATTEN_INDEX(pos, map->mapSize);
				int newIndex = DN_FLATTEN_INDEX(pos, size);

				newMap[newIndex] = DN_in_map_bounds(map, pos) ? map->map[oldIndex] : (DNchunkHandle){0};
			}

	DN_FREE(map->map);
	map->map = newMap;
	map->mapSize = size;

	//remove chunks that are no longer indexed:
	for(int i = 0; i < map->chunkCap; i++)
	{
		if(!map->chunks[i].used)
			continue;

		DNivec3 pos = map->chunks[i].pos;
		if(!DN_in_map_bounds(map, pos))
			_DN_clear_chunk(map, i);
	}

	//allocate new gpu buffer:
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glMapBufferID);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(DNchunkHandle) * size.x * size.y * size.z, map->map, GL_DYNAMIC_DRAW);
	if(glGetError() == GL_OUT_OF_MEMORY)
	{
		DN_ERROR_LOG("ERROR - UNABLE TO RESIZE MAP BUFFER\n");
		return false;
	}

	return true;
}

bool DN_set_max_chunks(DNmap* map, unsigned int num)
{
	DNchunk* newChunks = DN_REALLOC(map->chunks, sizeof(DNchunk) * num);

	if(!newChunks)
	{
		DN_ERROR_LOG("ERROR - UNABLE TO RESIZE CHUNK MEMORY\n");
		return false;
	}

	map->chunks = newChunks;
	map->chunkCap = num;
	return true;
}

bool DN_set_max_chunks_gpu(DNmap* map, unsigned int num)
{
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, map->glChunkBufferID);
	glBufferData(GL_SHADER_STORAGE_BUFFER, num * sizeof(DNchunk), NULL, GL_DYNAMIC_DRAW);
	if(glGetError() == GL_OUT_OF_MEMORY)
	{
		DN_ERROR_LOG("ERROR - UNABLE TO RESIZE GPU CHUNK BUFFER\n");
		return false;
	}

	return true;
}

bool DN_set_max_lighting_requests(DNmap* map, unsigned int num)
{
	DNuvec4* newRequests = DN_REALLOC(map->lightingRequests, sizeof(DNuvec4) * num);

	if(!newRequests)
	{
		DN_ERROR_LOG("ERROR - UNABLE TO RESIZE LIGHTING REQUEST MEMORY\n");
		return false;
	}

	map->lightingRequests = newRequests;
	map->lightingRequestCap = num;
	return true;
}

//--------------------------------------------------------------------------------------------------------------------------------//
//GPU-SIDE MAP SETTINGS:

void DN_set_materials(DNmaterialHandle min, unsigned int num)
{
	if(min + num > DN_MAX_VOXEL_MATERIALS)
	{
		DN_ERROR_LOG("ERROR - TRYING TO SET OUT OF BOUNDS MATERIALS\n");
		return;
	}

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, materialBuffer);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(DNmaterial) * num, &dnMaterials[min]);
}

//--------------------------------------------------------------------------------------------------------------------------------//
//MAP UTILITY:

bool DN_in_map_bounds(DNmap* map, DNivec3 pos)
{
	return pos.x < map->mapSize.x && pos.y < map->mapSize.y && pos.z < map->mapSize.z && pos.x >= 0 && pos.y >= 0 && pos.z >= 0;
}

bool DN_in_chunk_bounds(DNivec3 pos)
{
	return pos.x < DN_CHUNK_SIZE.x && pos.y < DN_CHUNK_SIZE.y && pos.z < DN_CHUNK_SIZE.z && pos.x >= 0 && pos.y >= 0 && pos.z >= 0;
}

DNvoxel DN_get_voxel(DNmap* map, DNivec3 chunkPos, DNivec3 localPos)
{
	return DN_decompress_voxel(DN_get_compressed_voxel(map, chunkPos, localPos));
}

DNcompressedVoxel DN_get_compressed_voxel(DNmap* map, DNivec3 chunkPos, DNivec3 localPos)
{
	return map->chunks[map->map[DN_FLATTEN_INDEX(chunkPos, map->mapSize)].index].voxels[localPos.x][localPos.y][localPos.z];
}

void DN_set_voxel(DNmap* map, DNivec3 chunkPos, DNivec3 localPos, DNvoxel voxel)
{
	DN_set_compressed_voxel(map, chunkPos, localPos, DN_compress_voxel(voxel));
}

void DN_set_compressed_voxel(DNmap* map, DNivec3 chunkPos, DNivec3 localPos, DNcompressedVoxel voxel)
{
	//add new chunk if the requested chunk doesn't yet exist:
	unsigned int mapIndex = DN_FLATTEN_INDEX(chunkPos, map->mapSize);
	if(map->map[mapIndex].flag == 0)
		DN_add_chunk(map, chunkPos);
	
	unsigned int chunkIndex = map->map[mapIndex].index;

	//change number of voxels in map:
	unsigned int oldMat = map->chunks[chunkIndex].voxels[localPos.x][localPos.y][localPos.z].albedo & 0xFF;
	unsigned int newMat = voxel.albedo & 0xFF;

	if(oldMat == 255 && newMat < 255) //if old voxel was empty and new one is not, increment the number of voxels
		map->chunks[chunkIndex].numVoxels++;
	else if(oldMat < 255 && newMat == 255) //if old voxel was not empty and new one is, decrement the number of voxels
	{
		map->chunks[chunkIndex].numVoxels--;

		//check if the chunk should be removed:
		if(map->chunks[chunkIndex].numVoxels <= 0)
		{
			DN_remove_chunk(map, chunkPos);
			return;
		}
	}

	//actually set new voxel:
	map->chunks[chunkIndex].voxels[localPos.x][localPos.y][localPos.z] = voxel;
	map->chunks[chunkIndex].updated = 1;
}

void DN_remove_voxel(DNmap* map, DNivec3 chunkPos, DNivec3 localPos)
{
	//change number of voxels in map (only if old voxel was solid)
	unsigned int chunkIndex = map->map[DN_FLATTEN_INDEX(chunkPos, map->mapSize)].index;
	if(DN_does_voxel_exist(map, chunkPos, localPos))
	{
		map->chunks[chunkIndex].numVoxels--;

		//remove chunk if no more voxels exist:
		if(map->chunks[chunkIndex].numVoxels <= 0)
		{
			DN_remove_chunk(map, chunkPos);
			return;
		}
	}

	//clear voxel:
	map->chunks[chunkIndex].voxels[localPos.x][localPos.y][localPos.z].albedo = UINT32_MAX; 
	map->chunks[chunkIndex].updated = 1;
}

bool DN_does_chunk_exist(DNmap* map, DNivec3 pos)
{
	return map->map[DN_FLATTEN_INDEX(pos, map->mapSize)].flag >= 1;
}

bool DN_does_voxel_exist(DNmap* map, DNivec3 chunkPos, DNivec3 localPos)
{
	unsigned int material = DN_get_compressed_voxel(map, chunkPos, localPos).albedo & 0xFF;
	return material < 255;
}

static int sign(float num)
{
	return (num > 0.0f) ? 1 : ((num < 0.0f) ? -1 : 0);
}

bool DN_step_map(DNmap* map, DNvec3 rayDir, DNvec3 rayPos, unsigned int maxSteps, DNivec3* hitPos, DNvoxel* hitVoxel, DNivec3* hitNormal)
{
	*hitNormal = (DNivec3){-1000, -1000, -1000};

	//scale to use voxel-level coordinates
	rayPos = DN_vec3_scale(rayPos, DN_CHUNK_SIZE.x);

	//utility:
	DNvec3 invRayDir = {1 / rayDir.x, 1 / rayDir.y, 1 / rayDir.z};
	DNvec3 rayDirSign = {sign(rayDir.x), sign(rayDir.y), sign(rayDir.z)};

	//create vars needed for dda:
	DNivec3 pos = {floor(rayPos.x), floor(rayPos.y), floor(rayPos.z)}; //the position in the voxel map
	DNvec3 deltaDist = {fabsf(invRayDir.x), fabsf(invRayDir.y), fabsf(invRayDir.z)}; //the distance the ray has to travel to move one unit in each direction
	DNivec3 rayStep = {rayDirSign.x, rayDirSign.y, rayDirSign.z}; //the direction the ray steps

	DNvec3 sideDist; //the total distance the ray has to travel to reach one additional unit in each direction (accounts for starting position as well)
	sideDist.x = (rayDirSign.x * (pos.x - rayPos.x) + (rayDirSign.x * 0.5) + 0.5) * deltaDist.x;
	sideDist.y = (rayDirSign.y * (pos.y - rayPos.y) + (rayDirSign.y * 0.5) + 0.5) * deltaDist.y;
	sideDist.z = (rayDirSign.z * (pos.z - rayPos.z) + (rayDirSign.z * 0.5) + 0.5) * deltaDist.z;

	unsigned int numSteps = 0;
	while(numSteps < maxSteps)
	{
		//check if in bounds:
		DNivec3 chunkPos;
		DNivec3 localPos;
		DN_separate_position(pos, &chunkPos, &localPos);

		if(pos.x < 0)
			chunkPos.x--;
		if(pos.y < 0)
			chunkPos.y--;
		if(pos.z < 0)
			chunkPos.z--;

		if(DN_in_map_bounds(map, chunkPos))
		{
			//check if voxel exists:
			if(DN_does_chunk_exist(map, chunkPos) && DN_does_voxel_exist(map, chunkPos, localPos))
			{
				*hitVoxel = DN_get_voxel(map, chunkPos, localPos);
				*hitPos = pos;
				return true;
			}
		}

		//iterate DDA algorithm:
		if (sideDist.x < sideDist.y) 
			if (sideDist.x < sideDist.z) 
			{
				sideDist.x += deltaDist.x;
				pos.x += rayStep.x;
				*hitNormal = (DNivec3){-rayStep.x, 0, 0};
			}
			else 
			{
				sideDist.z += deltaDist.z;
				pos.z += rayStep.z;
				*hitNormal = (DNivec3){0, 0, -rayStep.z};
			}
		else
			if (sideDist.y < sideDist.z) 
			{
				sideDist.y += deltaDist.y;
				pos.y += rayStep.y;
				*hitNormal = (DNivec3){0, -rayStep.y, 0};
			}
			else 
			{
				sideDist.z += deltaDist.z;
				pos.z += rayStep.z;
				*hitNormal = (DNivec3){0, 0, -rayStep.z};
			}

		numSteps++;
	}

	return false;
}

//--------------------------------------------------------------------------------------------------------------------------------//
//GENERAL UTILITY:

void DN_separate_position(DNivec3 pos, DNivec3* chunkPos, DNivec3* localPos)
{
	*chunkPos = (DNivec3){pos.x / DN_CHUNK_SIZE.x, pos.y / DN_CHUNK_SIZE.y, pos.z / DN_CHUNK_SIZE.z};
	*localPos = (DNivec3){pos.x % DN_CHUNK_SIZE.x, pos.y % DN_CHUNK_SIZE.y, pos.z % DN_CHUNK_SIZE.z};
}

DNvec3 DN_cam_dir(DNvec3 orient)
{
	DNmat3 rotate = DN_mat4_to_mat3(DN_mat4_rotate_euler(DN_MAT4_IDENTITY, orient));
	return DN_mat3_mult_vec3(rotate, (DNvec3){ 0.0f, 0.0f, 1.0f });
}

static GLuint _DN_encode_uint_RGBA(DNuvec4 val)
{
	val.x = val.x & 0xFF;
	val.y = val.y & 0xFF;
	val.z = val.z & 0xFF;
	val.w = val.w & 0xFF;
	return val.x << 24 | val.y << 16 | val.z << 8 | val.w;
}

static DNuvec4 _DN_decode_uint_RGBA(GLuint val)
{
	DNuvec4 res;
	res.x = (val >> 24) & 0xFF;
	res.y = (val >> 16) & 0xFF;
	res.z = (val >> 8) & 0xFF;
	res.w = (val) & 0xFF;
	return res;
}

DNcompressedVoxel DN_compress_voxel(DNvoxel voxel)
{
	DNcompressedVoxel res;
	voxel.albedo = DN_vec3_clamp(voxel.albedo, 0.0f, 1.0f);
	voxel.normal = DN_vec3_clamp(voxel.normal, -1.0f, 1.0f);
	DNuvec4 albedo = {(GLuint)(voxel.albedo.x * 255), (GLuint)(voxel.albedo.y * 255), (GLuint)(voxel.albedo.z * 255), voxel.material};
	DNuvec4 normal = {(GLuint)((voxel.normal.x * 0.5 + 0.5) * 255), (GLuint)((voxel.normal.y * 0.5 + 0.5) * 255), (GLuint)((voxel.normal.z * 0.5 + 0.5) * 255), voxel.mask & 0xFF};

	res.albedo = _DN_encode_uint_RGBA(albedo);
	res.normal = _DN_encode_uint_RGBA(normal);
	res.directLight = _DN_encode_uint_RGBA((DNuvec4){0, 0, 0, (voxel.mask >> 8) & 0xFF});
	res.specLight = _DN_encode_uint_RGBA((DNuvec4){0, 0, 0, 0});

	return res;
}

DNvoxel DN_decompress_voxel(DNcompressedVoxel voxel)
{
	DNvoxel res;
	DNuvec4 albedo = _DN_decode_uint_RGBA(voxel.albedo);
	DNuvec4 normal = _DN_decode_uint_RGBA(voxel.normal);
	DNuvec4 directLight = _DN_decode_uint_RGBA(voxel.directLight);

	res.albedo = DN_vec3_scale((DNvec3){albedo.x, albedo.y, albedo.z}, 0.00392156862);
	DNvec3 scaledNormal = DN_vec3_scale((DNvec3){normal.x, normal.y, normal.z}, 0.00392156862);
	res.normal = (DNvec3){(scaledNormal.x - 0.5) * 2.0, (scaledNormal.y - 0.5) * 2.0, (scaledNormal.z - 0.5) * 2.0};
	res.material = albedo.w;
	res.mask = normal.w | (directLight.w << 8);

	return res;
}

//--------------------------------------------------------------------------------------------------------------------------------//
//STATIC UTIL FUNCTIONS:

static bool _DN_gen_shader_storage_buffer(unsigned int* dest, size_t size)
{
	unsigned int buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer); //bind
	glBufferData(GL_SHADER_STORAGE_BUFFER, size, NULL, GL_DYNAMIC_DRAW); //allocate
	if(glGetError() == GL_OUT_OF_MEMORY)
	{	
		glDeleteBuffers(1, &buffer);
		return false;
	}

	*dest = buffer;

	return true;
}

static void _DN_clear_chunk(DNmap* map, unsigned int index)
{
	map->chunks[index].used = false;
	map->chunks[index].updated = false;
	map->chunks[index].numVoxels = 0;

	for(int z = 0; z < DN_CHUNK_SIZE.z; z++)
		for(int y = 0; y < DN_CHUNK_SIZE.y; y++)
			for(int x = 0; x < DN_CHUNK_SIZE.x; x++)
			{
				map->chunks[index].voxels[x][y][z].albedo = UINT32_MAX;
				map->chunks[index].indirectLight[x][y][z] = (DNvec4){0, 0, 0, 0};
			}
}