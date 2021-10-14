#include "ReflectionComp.h"
#include "../Define.h"

namespace cc {

ReflectionComp::~ReflectionComp() {

    CC_SAFE_DESTROY(_compShader);
    CC_SAFE_DESTROY(_compDescriptorSetLayout);
    CC_SAFE_DESTROY(_compPipelineLayout);
    CC_SAFE_DESTROY(_compPipelineState);
    CC_SAFE_DESTROY(_compDescriptorSet);

    CC_SAFE_DESTROY(_compDenoiseShader);
    CC_SAFE_DESTROY(_compDenoiseDescriptorSetLayout);
    CC_SAFE_DESTROY(_compDenoisePipelineLayout);
    CC_SAFE_DESTROY(_compDenoisePipelineState);
    CC_SAFE_DESTROY(_compDenoiseDescriptorSet);

    CC_SAFE_DESTROY(_localDescriptorSetLayout);

    CC_SAFE_DESTROY(_compConstantsBuffer);
}

namespace {
struct ConstantBuffer {
    Mat4 matView;
    Mat4 matViewProj;   // view projection矩阵
    Mat4 matProjInv;    // projection逆矩阵
    Vec4 viewPort;      // lighting viewport
    Vec2 texSize;       // 反射纹理大小
};
} // namespace

void ReflectionComp::applyTexSize(uint width, uint height, const Mat4 &matView,
                                  const Mat4& matViewProj, const Mat4& matProjInv,
                                  const Vec4 &viewPort) {
    uint globalWidth  = width;
    uint globalHeight = height;
    uint groupWidth   = this->getGroupSizeX();
    uint groupHeight  = this->getGroupSizeY();

    _dispatchInfo        = {(globalWidth - 1) / groupWidth + 1, (globalHeight - 1) / groupHeight + 1, 1};
    _denoiseDispatchInfo = {((globalWidth - 1) / 2) / groupWidth + 1, ((globalHeight - 1) / 2) / groupHeight + 1, 1};

    ConstantBuffer constants;
    constants.matView       = matView;
    constants.matViewProj   = matViewProj;
    constants.matProjInv    = matProjInv;
    constants.viewPort      = viewPort;
    constants.texSize       = {float(width), float(height)};
    constants.viewPort      = viewPort;

    if (_compConstantsBuffer) {
        _compConstantsBuffer->update(&constants, sizeof(constants));
    }
}

void ReflectionComp::init(gfx::Device *dev, uint groupSizeX, uint groupSizeY) {
    if (!dev->hasFeature(gfx::Feature::COMPUTE_SHADER)) return;

    _device           = dev;
    _groupSizeX       = groupSizeX;
    _groupSizeY       = groupSizeY;

    gfx::SamplerInfo samplerInfo;
    samplerInfo.minFilter = gfx::Filter::POINT;
    samplerInfo.magFilter = gfx::Filter::POINT;
    _sampler              = _device->getSampler(samplerInfo);

    uint maxInvocations = _device->getCapabilities().maxComputeWorkGroupInvocations;
    CCASSERT(_groupSizeX * _groupSizeY <= maxInvocations, "maxInvocations is too small");
    CC_LOG_INFO(" work group size: %dx%d", _groupSizeX, _groupSizeY);

    gfx::DescriptorSetLayoutInfo layoutInfo = {pipeline::localDescriptorSetLayout.bindings};
    _localDescriptorSetLayout               = _device->createDescriptorSetLayout(layoutInfo);

    gfx::GlobalBarrierInfo infoPre = {
        {
            gfx::AccessType::COLOR_ATTACHMENT_WRITE,
        },
        {
            gfx::AccessType::COMPUTE_SHADER_READ_TEXTURE,
        }};

    gfx::TextureBarrierInfo infoBeforeDenoise = {
        {
            gfx::AccessType::COMPUTE_SHADER_WRITE,
        },
        {
            gfx::AccessType::COMPUTE_SHADER_READ_TEXTURE,
        }};

    gfx::TextureBarrierInfo infoBeforeDenoise2 = {
        {
            gfx::AccessType::NONE,
        },
        {
            gfx::AccessType::COMPUTE_SHADER_WRITE,
        }};

    gfx::TextureBarrierInfo infoAfterDenoise = {
        {
            gfx::AccessType::COMPUTE_SHADER_WRITE,
        },
        {
            gfx::AccessType::FRAGMENT_SHADER_READ_TEXTURE,
        }};

    _barrierPre = _device->getGlobalBarrier(infoPre);
    _barrierBeforeDenoise.push_back(_device->getTextureBarrier(infoBeforeDenoise));
    _barrierBeforeDenoise.push_back(_device->getTextureBarrier(infoBeforeDenoise2));
    _barrierAfterDenoise.push_back(_device->getTextureBarrier(infoAfterDenoise));

    initReflectionRes();
    initDenoiseRes();
}

void ReflectionComp::initReflectionRes() {
    _compConstantsBuffer = _device->createBuffer({gfx::BufferUsage::UNIFORM,
                                                  gfx::MemoryUsage::DEVICE | gfx::MemoryUsage::HOST,
                                                  (sizeof(Mat4) * 3 + sizeof(Vec2) + sizeof(Vec4) + 15) / 16 * 16});

    ShaderSources<ComputeShaderSource> sources;
    sources.glsl4 = StringUtil::format(
        R"(
        layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

        layout(set = 0, binding = 0) uniform Constants
        {
            mat4 matView;
            mat4 matViewProj;
            mat4 matProjInv;
            vec4 viewPort;
            vec2 texSize;
        };
        layout(set = 0, binding = 1) uniform sampler2D lightingTex;
        layout(set = 0, binding = 2) uniform sampler2D worldPositionTex;
        layout(set = 0, binding = 3, rgba8) writeonly uniform lowp image2D reflectionTex;

        layout(set = 0, binding = 4, std140) uniform CCLocal
        {
            mat4 cc_matWorld;
            mat4 cc_matWorldIT;
            vec4 cc_lightingMapUVParam;
        };

        void main() {
            float _HorizontalPlaneHeightWS = 0.01;
            _HorizontalPlaneHeightWS = (cc_matWorld * vec4(0,0,0,1)).y;
            vec2 uv = vec2(gl_GlobalInvocationID.xy) / texSize;
            vec3 posWS = texture(worldPositionTex, uv).xyz;
            if(posWS.y <= _HorizontalPlaneHeightWS) return;

            vec3 reflectedPosWS = posWS;
            reflectedPosWS.y = reflectedPosWS.y - _HorizontalPlaneHeightWS;
            reflectedPosWS.y = reflectedPosWS.y * -1.0;
            reflectedPosWS.y = reflectedPosWS.y + _HorizontalPlaneHeightWS;


            vec4 reflectedPosCS = matViewProj * vec4(reflectedPosWS, 1);
            vec2 reflectedPosNDCxy = reflectedPosCS.xy / reflectedPosCS.w;//posCS -> posNDC
            vec2 reflectedScreenUV = reflectedPosNDCxy * 0.5 + 0.5; //posNDC

            vec2 earlyExitTest = abs(reflectedScreenUV - 0.5);
            if (earlyExitTest.x >= 0.5 || earlyExitTest.y >= 0.5) return;

            vec4 inputPixelSceneColor = texture(lightingTex, uv);
            imageStore(reflectionTex, ivec2(reflectedScreenUV * texSize), inputPixelSceneColor);
        })",
        _groupSizeX, _groupSizeY);
    sources.glsl3 = StringUtil::format(
        R"(
        layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

        layout(std140) uniform Constants
        {
            mat4 matView;
            mat4 matViewProj;
            mat4 matProjInv;
            vec4 viewPort;
            vec2 texSize;
        };
        uniform sampler2D lightingTex;
        uniform sampler2D worldPositionTex;
        layout(rgba8) writeonly uniform lowp image2D reflectionTex;

        layout(std140) uniform CCLocal
        {
            mat4 cc_matWorld;
            mat4 cc_matWorldIT;
            vec4 cc_lightingMapUVParam;
        };

        void main() {
            float _HorizontalPlaneHeightWS = 0.01;
            _HorizontalPlaneHeightWS = (cc_matWorld * vec4(0,0,0,1)).y;
            vec2 uv = vec2(gl_GlobalInvocationID.xy) / texSize;
            vec3 posWS = texture(worldPositionTex, uv).xyz;
            if(posWS.y <= _HorizontalPlaneHeightWS) return;

            vec3 reflectedPosWS = posWS;
            reflectedPosWS.y = reflectedPosWS.y - _HorizontalPlaneHeightWS;
            reflectedPosWS.y = reflectedPosWS.y * -1.0;
            reflectedPosWS.y = reflectedPosWS.y + _HorizontalPlaneHeightWS;

            vec4 reflectedPosCS = matViewProj * vec4(reflectedPosWS, 1);
            vec2 reflectedPosNDCxy = reflectedPosCS.xy / reflectedPosCS.w;//posCS -> posNDC
            vec2 reflectedScreenUV = reflectedPosNDCxy * 0.5 + 0.5; //posNDC

            vec2 earlyExitTest = abs(reflectedScreenUV - 0.5);
            if (earlyExitTest.x >= 0.5 || earlyExitTest.y >= 0.5) return;

            vec4 inputPixelSceneColor = texture(lightingTex, uv);
            imageStore(reflectionTex, ivec2(reflectedScreenUV * texSize), inputPixelSceneColor);
        })",
        _groupSizeX, _groupSizeY);
    // no compute support in GLES2

    gfx::ShaderInfo shaderInfo;
    shaderInfo.name   = "Compute ";
    shaderInfo.stages = {{gfx::ShaderStageFlagBit::COMPUTE, getAppropriateShaderSource(sources)}};
    shaderInfo.blocks = {
        {0, 0, "Constants", {
            {"matView", gfx::Type::MAT4, 1},
            {"matViewProj", gfx::Type::MAT4, 1},
            {"matProjInv", gfx::Type::MAT4, 1},
            {"viewPort", gfx::Type::FLOAT4, 1},
            {"texSize", gfx::Type::FLOAT2, 1},
            }, 1},
        {0, 4, "CCLocal", {{"cc_matWorld", gfx::Type::MAT4, 1}, {"cc_matWorldIT", gfx::Type::MAT4, 1}, {"cc_lightingMapUVParam", gfx::Type::FLOAT4, 1}}, 1}};
    shaderInfo.samplerTextures = {
        {0, 1, "lightingTex", gfx::Type::SAMPLER2D, 1},
        {0, 2, "worldPositionTex", gfx::Type::SAMPLER2D, 1}};
    shaderInfo.images = {
        {0, 3, "reflectionTex", gfx::Type::IMAGE2D, 1, gfx::MemoryAccessBit::WRITE_ONLY}};
    _compShader = _device->createShader(shaderInfo);

    gfx::DescriptorSetLayoutInfo dslInfo;
    dslInfo.bindings.push_back({0, gfx::DescriptorType::UNIFORM_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({1, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({2, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({3, gfx::DescriptorType::STORAGE_IMAGE, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({4, gfx::DescriptorType::UNIFORM_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE});

    _compDescriptorSetLayout = _device->createDescriptorSetLayout(dslInfo);
    _compDescriptorSet       = _device->createDescriptorSet({_compDescriptorSetLayout});

    _compPipelineLayout = _device->createPipelineLayout({{_compDescriptorSetLayout, _localDescriptorSetLayout}});

    gfx::PipelineStateInfo pipelineInfo;
    pipelineInfo.shader         = _compShader;
    pipelineInfo.pipelineLayout = _compPipelineLayout;
    pipelineInfo.bindPoint      = gfx::PipelineBindPoint::COMPUTE;

    _compPipelineState = _device->createPipelineState(pipelineInfo);
}

void ReflectionComp::initDenoiseRes() {
    ShaderSources<ComputeShaderSource> sources;
    sources.glsl4 = StringUtil::format(
        R"(
        layout(local_size_x = %d, local_size_y = %d, local_size_z = 1) in;
        layout(set = 0, binding = 0) uniform sampler2D reflectionTex;
        layout(set = 0, binding = 1) uniform samplerCube envMap;
        layout(set = 0, binding = 2) uniform sampler2D depth;
        layout(set = 0, binding = 3) uniform Constants
        {
            mat4 matView;
            mat4 matViewProj;
            mat4 matProjInv;
            vec4 viewPort;
            vec2 texSize;
        };

        layout(set = 0, binding = 4, std140) uniform CCLocal
        {
            mat4 cc_matWorld;
            mat4 cc_matWorldIT;
            vec4 cc_lightingMapUVParam;
        };

        layout(set = 1, binding = 12, rgba8) writeonly uniform lowp image2D denoiseTex;

        vec4 screen2Eye(vec3 coord) {
            vec4 ndc = vec4(
                2.0 * (coord.x - viewPort.x) / viewPort.z - 1.0,
                2.0 * (coord.y - viewPort.y) / viewPort.w - 1.0,
                (coord.z + 1.0) / 2.0,
                1.0
            );

            vec4 eye = matProjInv * ndc;
            eye = eye / eye.w;
            return eye;
        }

        vec3 calEnvmapUV(vec3 eyeCoord) {
            vec4 planeNornalWS = vec4(0, 1.0, 0, 1.0);
            vec3 planeNornalES = normalize((matView * planeNornalWS).xyz);
            vec3 incidence = normalize(-eyeCoord.xyz);
            vec3 reflection = normalize(reflect(incidence, planeNornalES));
            return normalize(incidence + reflection);
        }

        vec4 getEnvmap(ivec2 id) {
            vec2 screenSize = viewPort.zw;
            vec2 uv = vec2(float(id.x) / texSize.x, float(id.y) / texSize.y);
            float depthv = texture(depth, uv).x;
            vec2 screenPos = uv * screenSize;
            vec3 posES = screen2Eye(vec3(screenPos, depthv)).xyz;
            vec3 envmapUV = calEnvmapUV(posES);
            vec4 envValue = texture(envMap, envmapUV);
            return envValue;
        }

        void main() {
            ivec2 id = ivec2(gl_GlobalInvocationID.xy) * 2;

            vec4 center = texelFetch(reflectionTex, id + ivec2(0, 0), 0);
            vec4 right = texelFetch(reflectionTex, id + ivec2(0, 1), 0);
            vec4 bottom = texelFetch(reflectionTex, id + ivec2(1, 0), 0);
            vec4 bottomRight = texelFetch(reflectionTex, id + ivec2(1, 1), 0);

            vec4 best = center;
            best = right.a > best.a + 0.1 ? right : best;
            best = bottom.a > best.a + 0.1 ? bottom : best;
            best = bottomRight.a > best.a + 0.1 ? bottomRight : best;

            vec4 envValue = getEnvmap(id);

            vec4 res = best.a > center.a + 0.1 ? best : center;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(0, 0), res);

            res = best.a > right.a + 0.1 ? best : right;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(0, 1), res);

            res = best.a > bottom.a + 0.1 ? best : bottom;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(1, 0), res);

            res = best.a > bottomRight.a + 0.1 ? best : bottomRight;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(1, 1), res);

        })",
        _groupSizeX, _groupSizeY, pipeline::REFLECTIONSTORAGE::BINDING);
    sources.glsl3 = StringUtil::format(
        R"(
        layout(local_size_x = %d, local_size_y = %d, local_size_z = 1) in;
        uniform sampler2D reflectionTex;
        uniform samplerCube envMap;
        uniform sampler2D depth;
        layout(std140) uniform Constants
        {
            mat4 matView;
            mat4 matViewProj;
            mat4 matProjInv;
            vec4 viewPort;
            vec2 texSize;
        };

        layout(std140) uniform CCLocal
        {
            mat4 cc_matWorld;
            mat4 cc_matWorldIT;
            vec4 cc_lightingMapUVParam;
        };

        layout(rgba8) writeonly uniform lowp image2D denoiseTex;

        vec4 screen2Eye(vec3 coord) {
            vec4 ndc = vec4(
                2.0 * (coord.x - viewPort.x) / viewPort.z - 1.0,
                2.0 * (coord.y - viewPort.y) / viewPort.w - 1.0,
                2.0 * coord.z - 1.0,
                1.0
            );

            vec4 eye = matProjInv * ndc;
            eye = eye / eye.w;
            return eye;
        }

        vec3 calEnvmapUV(vec3 eyeCoord) {
            vec4 planeNornalWS = vec4(0, 1.0, 0, 1.0);
            vec3 planeNornalES = normalize((matView * planeNornalWS).xyz);
            vec3 incidence = normalize(-eyeCoord.xyz);
            vec3 reflection = normalize(reflect(incidence, planeNornalES));
            return normalize(incidence + reflection);
        }

        vec4 getEnvmap(ivec2 id) {
            vec2 screenSize = viewPort.zw;
            vec2 uv = vec2(float(id.x) / texSize.x, float(id.y) / texSize.y);
            float depthv = texture(depth, uv).x;
            vec2 screenPos = uv * screenSize;
            vec3 posES = screen2Eye(vec3(screenPos, depthv)).xyz;
            vec3 envmapUV = calEnvmapUV(posES);
            vec4 envValue = texture(envMap, envmapUV);
            return envValue;//vec4(depthv, 0, 0, 1);
        }

        void main() {
            ivec2 id = ivec2(gl_GlobalInvocationID.xy) * 2;

            vec4 center = texelFetch(reflectionTex, id + ivec2(0, 0), 0);
            vec4 right = texelFetch(reflectionTex, id + ivec2(0, 1), 0);
            vec4 bottom = texelFetch(reflectionTex, id + ivec2(1, 0), 0);
            vec4 bottomRight = texelFetch(reflectionTex, id + ivec2(1, 1), 0);

            vec4 best = center;
            best = right.a > best.a + 0.1 ? right : best;
            best = bottom.a > best.a + 0.1 ? bottom : best;
            best = bottomRight.a > best.a + 0.1 ? bottomRight : best;

            vec4 envValue = getEnvmap(id);

            vec4 res = best.a > center.a + 0.1 ? best : center;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(0, 0), res);

            res = best.a > right.a + 0.1 ? best : right;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(0, 1), res);

            res = best.a > bottom.a + 0.1 ? best : bottom;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(1, 0), res);

            res = best.a > bottomRight.a + 0.1 ? best : bottomRight;
            if (res.xyz == vec3(0, 0, 0)) {
                res = envValue;
            }
            imageStore(denoiseTex, id + ivec2(1, 1), res);
        })",
        _groupSizeX, _groupSizeY);
    // no compute support in GLES2

    gfx::ShaderInfo shaderInfo;
    shaderInfo.name            = "Compute";
    shaderInfo.stages          = {{gfx::ShaderStageFlagBit::COMPUTE, getAppropriateShaderSource(sources)}};
    shaderInfo.blocks = {
        {0, 3, "Constants", {
            {"matView", gfx::Type::MAT4, 1},
            {"matViewProj", gfx::Type::MAT4, 1},
            {"matProjInv", gfx::Type::MAT4, 1},
            {"viewPort", gfx::Type::FLOAT4, 1},
            {"texSize", gfx::Type::FLOAT2, 1},
            }, 1},
        {0, 4, "CCLocal", {{"cc_matWorld", gfx::Type::MAT4, 1}, {"cc_matWorldIT", gfx::Type::MAT4, 1}, {"cc_lightingMapUVParam", gfx::Type::FLOAT4, 1}}, 1}};

    shaderInfo.samplerTextures = {
        {0, 0, "reflectionTex", gfx::Type::SAMPLER2D, 1},
        {0, 1, "envMap", gfx::Type::SAMPLER_CUBE, 1},
        {0, 0, "depth", gfx::Type::SAMPLER2D, 1},
        };
    shaderInfo.images = {
        {1, 12, "denoiseTex", gfx::Type::IMAGE2D, 1, gfx::MemoryAccessBit::WRITE_ONLY}};
    _compDenoiseShader = _device->createShader(shaderInfo);

    gfx::DescriptorSetLayoutInfo dslInfo;
    dslInfo.bindings.push_back({0, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({1, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({2, gfx::DescriptorType::SAMPLER_TEXTURE, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({3, gfx::DescriptorType::UNIFORM_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE});
    dslInfo.bindings.push_back({4, gfx::DescriptorType::UNIFORM_BUFFER, 1, gfx::ShaderStageFlagBit::COMPUTE});
    _compDenoiseDescriptorSetLayout = _device->createDescriptorSetLayout(dslInfo);
    _compDenoisePipelineLayout      = _device->createPipelineLayout({{_compDenoiseDescriptorSetLayout, _localDescriptorSetLayout}});

    _compDenoiseDescriptorSet = _device->createDescriptorSet({_compDenoiseDescriptorSetLayout});

    gfx::PipelineStateInfo pipelineInfo;
    pipelineInfo.shader         = _compDenoiseShader;
    pipelineInfo.pipelineLayout = _compDenoisePipelineLayout;
    pipelineInfo.bindPoint      = gfx::PipelineBindPoint::COMPUTE;

    _compDenoisePipelineState = _device->createPipelineState(pipelineInfo);
}

template <typename T>
T &ReflectionComp::getAppropriateShaderSource(ShaderSources<T> &sources) {
    switch (_device->getGfxAPI()) {
        case gfx::API::GLES2:
            return sources.glsl1;
        case gfx::API::GLES3:
            return sources.glsl3;
        case gfx::API::METAL:
        case gfx::API::VULKAN:
            return sources.glsl4;
        default: break;
    }
    return sources.glsl4;
}

} // namespace cc
