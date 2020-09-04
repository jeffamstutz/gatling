#ifndef GATLING_SHADER_COMMON
#define GATLING_SHADER_COMMON

const float FLOAT_MAX = 3.402823466e38;
const float FLOAT_MIN = 1.175494351e-38;
const float PI = 3.1415926535897932384626433832795;

struct vertex
{
    /* pos.{x, y, z}, tex.u */
    vec4 field1;
    /* norm.{x, y, z}, tex.v */
    vec4 field2;
};

struct face
{
    uint v_0;
    uint v_1;
    uint v_2;
    uint mat_index;
};

struct material
{
    vec4 color;
    vec3 emission;
    float padding;
};

struct bvh_node
{
    /* Quantization frame. */
    vec3 p;                  /* 12 bytes */
    u8vec3 e;                /*  3 bytes */
    /* Indexing info. */
    uint8_t imask;           /*  1 byte  */
    uint child_index;        /*  4 bytes */
    uint face_index;         /*  4 bytes */
    uint meta[2];            /*  8 bytes */
    /* Child data. */
    u8vec4 q_lo_x[2];        /*  8 bytes */
    u8vec4 q_lo_y[2];        /*  8 bytes */
    u8vec4 q_lo_z[2];        /*  8 bytes */
    u8vec4 q_hi_x[2];        /*  8 bytes */
    u8vec4 q_hi_y[2];        /*  8 bytes */
    u8vec4 q_hi_z[2];        /*  8 bytes */
};

struct path_segment
{
    vec3 origin;
    uint pixel_index;
    vec3 dir;
    float pad_1;
    vec3 throughput;
    float pad_2;
};

struct hit_info
{
    vec3 pos;
    uint face_index;
    vec3 throughput;
    uint pixel_index;
    vec2 bc;
    vec2 padding;
};

layout(set=0, binding=0) queuefamilycoherent buffer BufferOutput
{
    uint pixels[];
};

layout(set=0, binding=1) buffer BufferPathSegments
{
    uint path_segment_write_counter;
    uint path_segment_read_counter;
    uint pad_1;
    uint pad_2;
    path_segment path_segments[];
};

layout(set=0, binding=2) readonly buffer BufferBvhNodes
{
    bvh_node bvh_nodes[];
};

layout(set=0, binding=3) readonly buffer BufferFaces
{
    face faces[];
};

layout(set=0, binding=4) readonly buffer BufferVertices
{
    vertex vertices[];
};

layout(set=0, binding=5) readonly buffer BufferMaterials
{
    material materials[];
};

layout(set=0, binding=6) buffer BufferHitInfos
{
    uint hit_write_counter;
    uint hit_read_counter;
    uint padding[2];
    hit_info hits[];
};

uint wang_hash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

float random_float_between_0_and_1(inout uint rng_state)
{
    rng_state ^= rng_state << 13u;
    rng_state ^= rng_state >> 17u;
    rng_state ^= rng_state << 5u;
    return float(rng_state) * (1.0 / 4294967296.0);
}

#endif
