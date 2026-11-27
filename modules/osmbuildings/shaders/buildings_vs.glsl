#version __CONTEXT__

#include "powerscaling/powerscaling_vs.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

uniform mat4 modelViewProjection;
uniform mat4 modelView;
uniform mat3 normalMatrix;

out vec3 vsNormal;
out vec2 vsTexCoord;
out vec4 vsPositionCameraSpace;

void main() {
  vsNormal = normalize(normalMatrix * inNormal);
  vsTexCoord = inTexCoord;

  vec4 positionClipSpace = modelViewProjection * vec4(inPosition, 1.0);
  vsPositionCameraSpace = modelView * vec4(inPosition, 1.0);

  gl_Position = z_normalization(positionClipSpace);
}