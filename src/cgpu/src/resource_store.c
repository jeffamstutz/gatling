/*
 * Copyright (C) 2019-2022 Pablo Delgado Krämer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "resource_store.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

void resource_store_create(resource_store* store,
                           uint32_t item_byte_size,
                           uint32_t initial_capacity)
{
  assert(initial_capacity != 0);
  handle_store_create(&store->handle_store);
  store->objects = NULL;
  store->object_count = 0;

  const uint32_t ptr_size = sizeof(void*);
  store->item_byte_size = (item_byte_size + ptr_size - 1) / ptr_size * ptr_size;
  store->objects = malloc(store->item_byte_size * initial_capacity);
  store->object_count = initial_capacity;
}

void resource_store_destroy(resource_store* store)
{
  handle_store_destroy(&store->handle_store);
  free(store->objects);
}

uint64_t resource_store_create_handle(resource_store* store)
{
  return handle_store_create_handle(&store->handle_store);
}

void resource_store_free_handle(resource_store* store, uint64_t handle)
{
  handle_store_free_handle(&store->handle_store, handle);
}

bool resource_store_get(resource_store* store, uint64_t handle, void** object)
{
  if (!handle_store_is_handle_valid(&store->handle_store, handle)) {
    return false;
  }

  const uint32_t index = handle_store_get_index(handle);

  if (index >= store->object_count)
  {
    const uint32_t new_count = store->object_count * 2;
    store->objects = realloc(
        store->objects,
        new_count * store->item_byte_size
    );
    store->object_count = new_count;
  }

  const uint32_t object_index = store->item_byte_size * index;
  uint8_t* objects_ptr = (uint8_t*) store->objects;
  *object = (void*) (objects_ptr + object_index);
  return true;
}
