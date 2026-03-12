param([string]$sourcePath)

$cppFile = "$sourcePath/RenderSystems/Vulkan/src/OgreVulkanDevice.cpp"
$hFile = "$sourcePath/RenderSystems/Vulkan/include/OgreVulkanDevice.h"

# 1. Force Vulkan 1.2
(Get-Content $cppFile) -replace 'apiVersion = VK_MAKE_VERSION\( 1, 0, 2 \)', 'apiVersion = VK_MAKE_VERSION( 1, 2, 0 )' | Set-Content $cppFile

# 2. Add RT Extensions to loop (find a unique marker)
$extMarker = 'deviceExtensions.push_back\( VK_AMD_SHADER_TRINARY_MINMAX_EXTENSION_NAME \);'
$rtExts = "`t`t`telse if( extensionName == VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME )`r`n" +
          "`t`t`t`tdeviceExtensions.push_back( VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME );`r`n" +
          "`t`t`telse if( extensionName == VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME )`r`n" +
          "`t`t`t`tdeviceExtensions.push_back( VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME );`r`n" +
          "`t`t`telse if( extensionName == VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME )`r`n" +
          "`t`t`t`tdeviceExtensions.push_back( VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME );`r`n" +
          "`t`t`telse if( extensionName == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME )`r`n" +
          "`t`t`t`tdeviceExtensions.push_back( VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME );"

(Get-Content $cppFile) -replace $extMarker, ($extMarker + "`r`n" + $rtExts) | Set-Content $cppFile

# 3. CreateDevice Surgery (Manual Chain)
# Inject the chain and sync features at the end
$createStart = 'makeVkStruct\( createInfo, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO \);'
$manualChain = '        VkPhysicalDeviceVulkan12Features deviceVulkan12Features;
        makeVkStruct( deviceVulkan12Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES );
        deviceVulkan12Features.bufferDeviceAddress = VK_TRUE;
        deviceVulkan12Features.shaderOutputViewportIndex = VK_TRUE;
        deviceVulkan12Features.shaderOutputLayer = VK_TRUE;
        deviceVulkan12Features.descriptorIndexing = VK_TRUE;
        deviceVulkan12Features.runtimeDescriptorArray = VK_TRUE;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR deviceAsFeatures;
        makeVkStruct( deviceAsFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR );
        deviceAsFeatures.accelerationStructure = VK_TRUE;
        deviceAsFeatures.pNext = &deviceVulkan12Features;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR deviceRtFeatures;
        makeVkStruct( deviceRtFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR );
        deviceRtFeatures.rayTracingPipeline = VK_TRUE;
        deviceRtFeatures.pNext = &deviceAsFeatures;

        VkPhysicalDeviceFeatures2 deviceFeatures2;
        makeVkStruct( deviceFeatures2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 );
        deviceFeatures2.features = mDeviceFeatures; 
        deviceFeatures2.features.shaderInt64 = VK_TRUE;
        deviceFeatures2.features.shaderFloat64 = VK_TRUE;
        deviceFeatures2.pNext = &deviceRtFeatures;
'
(Get-Content $cppFile) -replace $createStart, ($manualChain + "`r`n        " + $createStart) | Set-Content $cppFile

# Replace pNext and check result
(Get-Content $cppFile) -replace 'createInfo.pNext = NULL;', 'createInfo.pNext = &deviceFeatures2;' | Set-Content $cppFile
(Get-Content $cppFile) -replace 'createInfo.pEnabledFeatures = &mDeviceFeatures;', 'createInfo.pEnabledFeatures = NULL;' | Set-Content $cppFile

$syncFeats = '        checkVkResult( this, result, "vkCreateDevice (PRISM Mode)" );
        mDeviceExtraFeatures.accelerationStructure = deviceAsFeatures.accelerationStructure;
        mDeviceExtraFeatures.rayTracingPipeline = deviceRtFeatures.rayTracingPipeline;
        mDeviceExtraFeatures.bufferDeviceAddress = deviceVulkan12Features.bufferDeviceAddress;
        mDeviceExtraFeatures.runtimeDescriptorArray = deviceVulkan12Features.runtimeDescriptorArray;'

(Get-Content $cppFile) -replace 'checkVkResult\( this, result, "vkCreateDevice" \);', $syncFeats | Set-Content $cppFile

# 4. Header member injection
(Get-Content $hFile) -replace 'VkBool32 pipelineCreationCacheControl;', 'VkBool32 pipelineCreationCacheControl;`r`n            VkBool32 rayTracingPipeline; VkBool32 accelerationStructure; VkBool32 bufferDeviceAddress; VkBool32 runtimeDescriptorArray;' | Set-Content $hFile
