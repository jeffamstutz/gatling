/* Möller-Trumbore triangle intersection. */
bool test_face(
    in RayInfo ray,
    in vec3 p0,
    in vec3 p1,
    in vec3 p2,
    out float t,
    out vec2 bc)
{
    vec3 e1 = p1 - p0;
    vec3 e2 = p2 - p0;

    vec3 p = cross(ray.dir, e2);
    float det = dot(e1, p);

    if (abs(det) < TRI_EPS) {
        return false;
    }

    float inv_det = 1.0 / det;
    vec3 tvec = ray.origin - p0;

    bc.x = dot(tvec, p) * inv_det;
    if (bc.x < 0.0 || bc.x > 1.0) {
        return false;
    }

    vec3 q = cross(tvec, e1);
    bc.y = dot(ray.dir, q) * inv_det;
    if (bc.y < 0.0 || (bc.x + bc.y > 1.0)) {
        return false;
    }

    t = dot(e2, q) * inv_det;
    if (t <= 0.0 || t >= ray.tmax) {
        return false;
    }

    return true;
}

#ifdef BVH_ENABLED

uint sign_to_byte_mask4(uint a)
{
    a = a & 0x80808080;
    a = a + a - (a >> 7);
    return a;
}

uint extract_byte(uint num, uint byte_idx)
{
    return (num >> (byte_idx * 8)) & 0xFF;
}

vec4 uint_unpack_vec4(uint u)
{
    return vec4(extract_byte(u, 0), extract_byte(u, 1), extract_byte(u, 2), extract_byte(u, 3));
}

bool find_hit_closest(inout RayInfo ray, out Hit_info hit)
{
    vec3 inv_dir = 1.0 / ray.dir;

    uvec3 oct_inv = mix(uvec3(0), uvec3(4, 2, 1), greaterThanEqual(ray.dir, vec3(0.0)));
    uint oct_inv4 = (oct_inv.x | oct_inv.y | oct_inv.z) * 0x01010101;

    uvec2 node_group = uvec2(0, 0x80000000);

    uvec2 stack[MAX_STACK_SIZE];
    uint stack_size = 0;

    hit.face_idx = 0xFFFFFFFF;
#if AOV_ID == AOV_ID_DEBUG_BVH_STEPS
    hit.bvh_steps = 0;
#elif AOV_ID == AOV_ID_DEBUG_TRI_TESTS
    hit.tri_tests = 0;
#endif

    while (true)
    {
        uvec2 face_group = uvec2(0, 0);

        if (node_group.y <= 0x00FFFFFF)
        {
            face_group = node_group;
            node_group = uvec2(0, 0);
        }
        else
        {
#if AOV_ID == AOV_ID_DEBUG_BVH_STEPS
            hit.bvh_steps++;
#endif
            uint child_bit_idx = findMSB(node_group.y);
            uint slot_index = (child_bit_idx - 24) ^ (oct_inv4 & 0xFF);
            uint rel_idx = bitCount(node_group.y & ~(0xFFFFFFFFu << slot_index));
            uint child_node_idx = node_group.x + rel_idx;

            node_group.y &= ~(1u << child_bit_idx);

            if (node_group.y > 0x00FFFFFF)
            {
                ASSERT(stack_size < MAX_STACK_SIZE, "Error: traversal stack size exceeds maximum capacity\n");
                stack[stack_size] = node_group;
                stack_size++;
            }

            bvh_node node = bvh_nodes[child_node_idx];

            node_group.x = node.f2.x;
            face_group = uvec2(node.f2.y, 0);

            uvec3 node_e = uvec3(extract_byte(node.f1.w, 0), extract_byte(node.f1.w, 1), extract_byte(node.f1.w, 2));
            vec3 local_inv_dir = uintBitsToFloat(node_e << 23) * inv_dir;
            vec3 p = uintBitsToFloat(node.f1.xyz);
            vec3 local_orig = (p - ray.origin) * inv_dir;

            uint hitmask = 0;

            [[unroll, dependency_infinite]]
            for (uint passIdx = 0; passIdx < 2; ++passIdx)
            {
                uint meta4 = (passIdx == 0) ? node.f2.z : node.f2.w;
                uint is_inner4 = (meta4 & (meta4 << 1)) & 0x10101010;
                uint inner_mask4 = sign_to_byte_mask4(is_inner4 << 3);
                uint bit_index4 = (meta4 ^ (oct_inv4 & inner_mask4)) & 0x1F1F1F1F;
                uint child_bits4 = (meta4 >> 5) & 0x07070707;

                bool x_lt_0 = (inv_dir.x < 0.0);
                bool y_lt_0 = (inv_dir.y < 0.0);
                bool z_lt_0 = (inv_dir.z < 0.0);

                uint q_lo_x = (passIdx == 0) ? node.f3.x : node.f3.y;
                uint q_hi_x = (passIdx == 0) ? node.f4.z : node.f4.w;
                uint q_lo_y = (passIdx == 0) ? node.f3.z : node.f3.w;
                uint q_hi_y = (passIdx == 0) ? node.f5.x : node.f5.y;
                uint q_lo_z = (passIdx == 0) ? node.f4.x : node.f4.y;
                uint q_hi_z = (passIdx == 0) ? node.f5.z : node.f5.w;

                vec4 s_q_lo_x = uint_unpack_vec4(x_lt_0 ? q_hi_x : q_lo_x);
                vec4 s_q_hi_x = uint_unpack_vec4(x_lt_0 ? q_lo_x : q_hi_x);
                vec4 s_q_lo_y = uint_unpack_vec4(y_lt_0 ? q_hi_y : q_lo_y);
                vec4 s_q_hi_y = uint_unpack_vec4(y_lt_0 ? q_lo_y : q_hi_y);
                vec4 s_q_lo_z = uint_unpack_vec4(z_lt_0 ? q_hi_z : q_lo_z);
                vec4 s_q_hi_z = uint_unpack_vec4(z_lt_0 ? q_lo_z : q_hi_z);

                vec4 t_min_x = local_inv_dir.x * s_q_lo_x + local_orig.x;
                vec4 t_max_x = local_inv_dir.x * s_q_hi_x + local_orig.x;
                vec4 t_min_y = local_inv_dir.y * s_q_lo_y + local_orig.y;
                vec4 t_max_y = local_inv_dir.y * s_q_hi_y + local_orig.y;
                vec4 t_min_z = local_inv_dir.z * s_q_lo_z + local_orig.z;
                vec4 t_max_z = local_inv_dir.z * s_q_hi_z + local_orig.z;

                [[unroll, dependency_infinite]]
                for (uint child_idx = 0; child_idx < 4; ++child_idx)
                {
                    float bmin = max(max(t_min_x[child_idx], t_min_y[child_idx]), max(t_min_z[child_idx], 0.0));
                    float bmax = min(min(t_max_x[child_idx], t_max_y[child_idx]), min(t_max_z[child_idx], ray.tmax));

                    bool is_intersected = bmin <= bmax;

                    if (!is_intersected)
                    {
                        continue;
                    }

                    uint child_bits = extract_byte(child_bits4, child_idx);
                    uint bit_index = extract_byte(bit_index4, child_idx);
                    hitmask |= (child_bits << bit_index);
                }
            }

            uint node_imask = extract_byte(node.f1.w, 3);
            node_group.y = (hitmask & 0xFF000000) | node_imask;
            face_group.y = (hitmask & 0x00FFFFFF);
        }

#ifdef TRIANGLE_POSTPONING
        uint active_inv_count1 = subgroupBallotBitCount(subgroupBallot(true));
#endif

        while (face_group.y != 0)
        {
#ifdef TRIANGLE_POSTPONING
            uint threshold = uint(active_inv_count1 * POSTPONE_RATIO);

            uint active_inv_count2 = subgroupBallotBitCount(subgroupBallot(true));

            if (active_inv_count2 < threshold)
            {
                ASSERT(stack_size < MAX_STACK_SIZE, "Error: traversal stack size exceeds maximum capacity\n");
                stack[stack_size] = face_group;
                stack_size++;
                break;
            }
#endif

            uint face_rel_index = findMSB(face_group.y);

            face_group.y &= ~(1u << face_rel_index);

            uint face_index = face_group.x + face_rel_index;

            face f = faces[face_index];
            vec3 p0 = vertices[f.v_0].field1.xyz;
            vec3 p1 = vertices[f.v_1].field1.xyz;
            vec3 p2 = vertices[f.v_2].field1.xyz;

            float temp_t;
            vec2 temp_bc;
            bool has_hit = test_face(ray, p0, p1, p2, temp_t, temp_bc);

            if (has_hit)
            {
                ray.tmax = temp_t;
                hit.bc = temp_bc;
                hit.face_idx = face_index;
            }

#if AOV_ID == AOV_ID_DEBUG_TRI_TESTS
            hit.tri_tests++;
#endif
        }

        if (node_group.y > 0x00FFFFFF)
        {
            continue;
        }

        if (stack_size > 0)
        {
            stack_size--;
            node_group = stack[stack_size];
            continue;
        }

        if (hit.face_idx != 0xFFFFFFFF)
        {
            return true;
        }

        return false;
    }
}

bool find_hit_any(in RayInfo ray)
{
    vec3 inv_dir = 1.0 / ray.dir;

    uvec3 oct_inv = mix(uvec3(0), uvec3(4, 2, 1), greaterThanEqual(ray.dir, vec3(0.0)));
    uint oct_inv4 = (oct_inv.x | oct_inv.y | oct_inv.z) * 0x01010101;

    uvec2 node_group = uvec2(0, 0x80000000);

    uvec2 stack[MAX_STACK_SIZE];
    uint stack_size = 0;

    while (true)
    {
        uvec2 face_group = uvec2(0, 0);

        if (node_group.y <= 0x00FFFFFF)
        {
            face_group = node_group;
            node_group = uvec2(0, 0);
        }
        else
        {
            uint child_bit_idx = findMSB(node_group.y);
            uint slot_index = (child_bit_idx - 24) ^ (oct_inv4 & 0xFF);
            uint rel_idx = bitCount(node_group.y & ~(0xFFFFFFFFu << slot_index));
            uint child_node_idx = node_group.x + rel_idx;

            node_group.y &= ~(1u << child_bit_idx);

            if (node_group.y > 0x00FFFFFF)
            {
                stack[stack_size] = node_group;
                stack_size++;
            }

            bvh_node node = bvh_nodes[child_node_idx];

            node_group.x = node.f2.x;
            face_group = uvec2(node.f2.y, 0);

            uvec3 node_e = uvec3(extract_byte(node.f1.w, 0), extract_byte(node.f1.w, 1), extract_byte(node.f1.w, 2));
            vec3 local_inv_dir = uintBitsToFloat(node_e << 23) * inv_dir;
            vec3 p = uintBitsToFloat(node.f1.xyz);
            vec3 local_orig = (p - ray.origin) * inv_dir;

            uint hitmask = 0;

            [[unroll, dependency_infinite]]
            for (uint passIdx = 0; passIdx < 2; ++passIdx)
            {
                uint meta4 = (passIdx == 0) ? node.f2.z : node.f2.w;
                uint is_inner4 = (meta4 & (meta4 << 1)) & 0x10101010;
                uint inner_mask4 = sign_to_byte_mask4(is_inner4 << 3);
                uint bit_index4 = (meta4 ^ (oct_inv4 & inner_mask4)) & 0x1F1F1F1F;
                uint child_bits4 = (meta4 >> 5) & 0x07070707;

                bool x_lt_0 = (inv_dir.x < 0.0);
                bool y_lt_0 = (inv_dir.y < 0.0);
                bool z_lt_0 = (inv_dir.z < 0.0);

                uint q_lo_x = (passIdx == 0) ? node.f3.x : node.f3.y;
                uint q_hi_x = (passIdx == 0) ? node.f4.z : node.f4.w;
                uint q_lo_y = (passIdx == 0) ? node.f3.z : node.f3.w;
                uint q_hi_y = (passIdx == 0) ? node.f5.x : node.f5.y;
                uint q_lo_z = (passIdx == 0) ? node.f4.x : node.f4.y;
                uint q_hi_z = (passIdx == 0) ? node.f5.z : node.f5.w;

                vec4 s_q_lo_x = uint_unpack_vec4(x_lt_0 ? q_hi_x : q_lo_x);
                vec4 s_q_hi_x = uint_unpack_vec4(x_lt_0 ? q_lo_x : q_hi_x);
                vec4 s_q_lo_y = uint_unpack_vec4(y_lt_0 ? q_hi_y : q_lo_y);
                vec4 s_q_hi_y = uint_unpack_vec4(y_lt_0 ? q_lo_y : q_hi_y);
                vec4 s_q_lo_z = uint_unpack_vec4(z_lt_0 ? q_hi_z : q_lo_z);
                vec4 s_q_hi_z = uint_unpack_vec4(z_lt_0 ? q_lo_z : q_hi_z);

                vec4 t_min_x = local_inv_dir.x * s_q_lo_x + local_orig.x;
                vec4 t_max_x = local_inv_dir.x * s_q_hi_x + local_orig.x;
                vec4 t_min_y = local_inv_dir.y * s_q_lo_y + local_orig.y;
                vec4 t_max_y = local_inv_dir.y * s_q_hi_y + local_orig.y;
                vec4 t_min_z = local_inv_dir.z * s_q_lo_z + local_orig.z;
                vec4 t_max_z = local_inv_dir.z * s_q_hi_z + local_orig.z;

                [[unroll, dependency_infinite]]
                for (uint child_idx = 0; child_idx < 4; ++child_idx)
                {
                    float bmin = max(max(t_min_x[child_idx], t_min_y[child_idx]), max(t_min_z[child_idx], 0.0));
                    float bmax = min(min(t_max_x[child_idx], t_max_y[child_idx]), min(t_max_z[child_idx], ray.tmax));

                    bool is_intersected = bmin <= bmax;

                    if (!is_intersected)
                    {
                        continue;
                    }

                    uint child_bits = extract_byte(child_bits4, child_idx);
                    uint bit_index = extract_byte(bit_index4, child_idx);
                    hitmask |= (child_bits << bit_index);
                }
            }

            uint node_imask = extract_byte(node.f1.w, 3);
            node_group.y = (hitmask & 0xFF000000) | node_imask;
            face_group.y = (hitmask & 0x00FFFFFF);
        }

        uint active_inv_count1 = subgroupBallotBitCount(subgroupBallot(true));

        while (face_group.y != 0)
        {
            uint threshold = uint(active_inv_count1 * POSTPONE_RATIO);

            uint active_inv_count2 = subgroupBallotBitCount(subgroupBallot(true));

            if (active_inv_count2 < threshold)
            {
                stack[stack_size] = face_group;
                stack_size++;
                break;
            }

            uint face_rel_index = findMSB(face_group.y);

            face_group.y &= ~(1u << face_rel_index);

            face f = faces[face_group.x + face_rel_index];
            vec3 p0 = vertices[f.v_0].field1.xyz;
            vec3 p1 = vertices[f.v_1].field1.xyz;
            vec3 p2 = vertices[f.v_2].field1.xyz;

            float temp_t;
            vec2 temp_bc;
            bool has_hit = test_face(ray, p0, p1, p2, temp_t, temp_bc);

            if (has_hit)
            {
                return true;
            }
        }

        if (node_group.y > 0x00FFFFFF)
        {
            continue;
        }

        if (stack_size > 0)
        {
            stack_size--;
            node_group = stack[stack_size];
            continue;
        }

        return false;
    }
}

#else

bool find_hit_closest(inout RayInfo ray, out Hit_info hit)
{
    hit.face_idx = 0xFFFFFFFF;
#if AOV_ID == AOV_ID_DEBUG_TRI_TESTS
    hit.tri_tests = 0;
#endif

    for (int face_index = 0; face_index < FACE_COUNT; face_index++)
    {
        face f = faces[face_index];
        vec3 p0 = vertices[f.v_0].field1.xyz;
        vec3 p1 = vertices[f.v_1].field1.xyz;
        vec3 p2 = vertices[f.v_2].field1.xyz;

        float temp_t;
        vec2 temp_bc;
        bool has_hit = test_face(ray, p0, p1, p2, temp_t, temp_bc);

#if AOV_ID == AOV_ID_DEBUG_TRI_TESTS
        hit.tri_tests++;
#endif

        if (has_hit)
        {
            ray.tmax = temp_t;
            hit.bc = temp_bc;
            hit.face_idx = face_index;
        }
    }

    if (hit.face_idx == 0xFFFFFFFF)
    {
        return false;
    }

    return true;
}

bool find_hit_any(in RayInfo ray)
{
    for (int face_index = 0; face_index < FACE_COUNT; face_index++)
    {
        face f = faces[face_index];
        vec3 p0 = vertices[f.v_0].field1.xyz;
        vec3 p1 = vertices[f.v_1].field1.xyz;
        vec3 p2 = vertices[f.v_2].field1.xyz;

        float temp_t;
        vec2 temp_bc;
        bool has_hit = test_face(ray, p0, p1, p2, temp_t, temp_bc);

        if (has_hit)
        {
            return true;
        }
    }

    return false;
}

#endif