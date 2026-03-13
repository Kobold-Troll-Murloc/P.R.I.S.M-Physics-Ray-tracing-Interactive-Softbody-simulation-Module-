/// PRISM: Custom G-Buffer MRT output piece
/// OGRE Next Vulkan path 는 DefaultBodyPS 에서 PixelData 구조체를 사용.
/// - nNormal (GLSLES 전용 글로벌) 대신 pixelData.normal 사용
/// - ROUGHNESS 매크로 대신 pixelData.perceptualRoughness 사용
/// - metallic proxy: pixelData.F0.x (metallic_workflow 시 metalness 반영)
///
/// MRT attachment 1 (gbuffer_normal)   → view-space normal [0,1]
/// MRT attachment 2 (gbuffer_material) → r=roughness, g=F0.x(metallic proxy)

@piece( custom_ps_uniformDeclaration )
@property( !hlms_shadowcaster )
layout(location = 1) out vec4 outGBufNormal;
layout(location = 2) out vec4 outGBufMaterial;
@end
@end

@piece( custom_ps_posExecution )
@property( !hlms_shadowcaster )
@property( hlms_normal || hlms_qtangent )
{
    /// Normal (attachment 1): view-space normal → [0,1]
    outGBufNormal = vec4( vec3(pixelData.normal) * 0.5 + 0.5, 1.0 );

    /// Material (attachment 2): r=roughness, g=metallic-proxy(F0.x), b=0, a=1
    float prismMetallicProxy = 0.0;
    @property( !fresnel_scalar )
        prismMetallicProxy = float( pixelData.F0 );    // scalar F0 (SpecularWorkflow: ~0.04)
    @else
        prismMetallicProxy = float( pixelData.F0.x );  // vec3 F0 (MetallicWorkflow: high for metal)
    @end
    outGBufMaterial = vec4( float(pixelData.perceptualRoughness), prismMetallicProxy, 0.0, 1.0 );
}
@else
{
    /// 법선 없는 오브젝트 (드문 경우) – 플레이스홀더
    outGBufNormal   = vec4( 0.5, 0.5, 1.0, 1.0 );
    outGBufMaterial = vec4( 0.5, 0.0, 0.0, 1.0 );
}
@end
@end
@end
