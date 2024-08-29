#extension GL_OES_EGL_image_external : require

#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform samplerExternalOES tex;
uniform float alpha;

void main() {
	gl_FragColor = texture2D(tex, v_texcoord) * alpha;
}
