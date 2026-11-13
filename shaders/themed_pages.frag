// A PDF's pages in the theme's colours: white paper becomes the theme's
// background and black ink its text, the colours of pictures kept around
// that lightness.
#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 paper;
    vec4 ink;
};

layout(binding = 1) uniform sampler2D source;

void main()
{
    vec4 color = texture(source, qt_TexCoord0);
    vec3 rgb = color.a > 0.0 ? color.rgb / color.a : vec3(1.0);
    float lightness = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3 shade = mix(ink.rgb, paper.rgb, lightness) + (rgb - vec3(lightness));
    fragColor = vec4(clamp(shade, 0.0, 1.0), 1.0) * color.a * qt_Opacity;
}
