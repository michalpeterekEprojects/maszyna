float calc_shadow()
{
#if SHADOWMAP_ENABLED
	vec3 coords = f_light_pos.xyz / f_light_pos.w;
	
	if (coords.z < 0.0f)
		return 1.0f;
	
	//sampler PCF
	//float shadow = texture(shadowmap, coords.xyz);

	//sampler PCF + PCF
	float shadow = 0.0;
	vec2 texel = 1.0 / textureSize(shadowmap, 0);
	for (float y = -1.5; y <= 1.5; y += 1.0)
		for (float x = -1.5; x <= 1.5; x += 1.0)
			shadow += texture(shadowmap, coords.xyz + vec3(vec2(x, y) * texel, 0.0));
	shadow /= 16.0;
		
	return 1.0 - shadow;
#else
    return 1.0;
#endif
}

vec3 apply_fog(vec3 color)
{
	float sun_amount = 0.0;
	if (lights_count >= 1U && lights[0].type == LIGHT_DIR)
		sun_amount = max(dot(normalize(f_pos.xyz), normalize(-lights[0].dir)), 0.0);
	vec3 fog_color_v = mix(fog_color, lights[0].color, pow(sun_amount, 30.0));
	float fog_amount_v = 1.0 - min(1.0, exp(-length(f_pos.xyz) * fog_density));
	return mix(color, fog_color_v, fog_amount_v);
}

// [0] - diffuse, [1] - specular
// do magic here
vec2 calc_light(vec3 light_dir)
{
#ifdef NORMALMAP
	vec3 normal = f_tbn * normalize(texture(normalmap, f_coord).rgb * 2.0 - 1.0);
#else
	vec3 normal = normalize(f_normal);
#endif
	vec3 view_dir = normalize(vec3(0.0f, 0.0f, 0.0f) - f_pos.xyz);
	vec3 halfway_dir = normalize(light_dir + view_dir);
	
	float diffuse_v = max(dot(normal, light_dir), 0.0);
	float specular_v = pow(max(dot(normal, halfway_dir), 0.0), 15.0);

	return vec2(diffuse_v, specular_v);
}

vec2 calc_point_light(light_s light)
{
	vec3 light_dir = normalize(light.pos - f_pos.xyz);
	vec2 val = calc_light(light_dir);
	
	float distance = length(light.pos - f_pos.xyz);
	float atten = 1.0f / (1.0f + light.linear * distance + light.quadratic * (distance * distance));
	
	return val * atten;
}

vec2 calc_spot_light(light_s light)
{
	vec3 light_dir = normalize(light.pos - f_pos.xyz);
	
	float theta = dot(light_dir, normalize(-light.dir));
	float epsilon = light.in_cutoff - light.out_cutoff;
	float intensity = clamp((theta - light.out_cutoff) / epsilon, 0.0, 1.0);

	vec2 point = calc_point_light(light);	
	return point * intensity;
}

vec2 calc_dir_light(light_s light)
{
	vec3 light_dir = normalize(-light.dir);
	return calc_light(light_dir);
}