#include "effect_smaa.hpp"

#include <cstring>

#include "image_view.hpp"
#include "descriptor_set.hpp"
#include "buffer.hpp"
#include "renderpass.hpp"
#include "graphics_pipeline.hpp"
#include "framebuffer.hpp"
#include "shader.hpp"
#include "sampler.hpp"
#include "image.hpp"

#include "AreaTex.h"
#include "SearchTex.h"

#ifndef ASSERT_VULKAN
#define ASSERT_VULKAN(val)\
        if(val!=VK_SUCCESS)\
        {\
            throw std::runtime_error("ASSERT_VULKAN failed " + std::to_string(val));\
        }
#endif

namespace vkBasalt
{
    typedef struct {
        float screenWidth;
        float screenHeight;
        float reverseScreenWidth;
        float reverseScreenHeight;
        float threshold;
        int32_t maxSearchSteps;
        int32_t maxSearchStepsDiag;
        int32_t cornerRounding;
    } SmaaOptions;



    SmaaEffect::SmaaEffect(VkPhysicalDevice physicalDevice, VkLayerInstanceDispatchTable instanceDispatchTable, VkDevice device, VkLayerDispatchTable dispatchTable, VkFormat format,  VkExtent2D imageExtent, std::vector<VkImage> inputImages, std::vector<VkImage> outputImages, std::shared_ptr<vkBasalt::Config> pConfig, VkQueue queue, VkCommandPool commandPool)
    {
        std::string smaaEdgeVertexFile        = "smaa_edge.vert.spv";
        std::string smaaEdgeLumaFragmentFile  = "smaa_edge_luma.frag.spv";
        std::string smaaEdgeColorFragmentFile = "smaa_edge_color.frag.spv";
        std::string smaaBlendVertexFile       = "smaa_blend.vert.spv";
        std::string smaaBlendFragmentFile     = "smaa_blend.frag.spv";
        std::string smaaNeighborVertexFile    = "smaa_neighbor.vert.spv";
        std::string smaaNeighborFragmentFile  = "smaa_neighbor.frag.spv";
        std::cout << "in creating SmaaEffect " << std::endl;

        this->physicalDevice = physicalDevice;
        this->instanceDispatchTable = instanceDispatchTable;
        this->device = device;
        this->dispatchTable = dispatchTable;
        this->format = format;
        this->imageExtent = imageExtent;
        this->inputImages = inputImages;
        this->outputImages = outputImages;
        this->pConfig = pConfig;

        //create Images for the first and second pass at once -> less memory fragmentation
        std::vector<VkImage> edgeAndBlendImages= createImages(instanceDispatchTable,
                                                               device,
                                                               dispatchTable,
                                                               physicalDevice,
                                                               inputImages.size()*2,
                                                               {imageExtent.width, imageExtent.height, 1},
                                                               VK_FORMAT_B8G8R8A8_UNORM,//TODO search for format and save it
                                                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                               imageMemory);

        edgeImages  = std::vector<VkImage>(edgeAndBlendImages.begin(), edgeAndBlendImages.begin() + edgeAndBlendImages.size()/2);
        blendImages = std::vector<VkImage>(edgeAndBlendImages.begin() + edgeAndBlendImages.size()/2, edgeAndBlendImages.end());

        inputImageViews = createImageViews(device, dispatchTable, format, inputImages);
        std::cout << "after creating input ImageViews" << std::endl;
        edgeImageViews = createImageViews(device, dispatchTable, VK_FORMAT_B8G8R8A8_UNORM, edgeImages);
        std::cout << "after creating edge  ImageViews" << std::endl;
        blendImageViews = createImageViews(device, dispatchTable, VK_FORMAT_B8G8R8A8_UNORM, blendImages);
        std::cout << "after creating blend ImageViews" << std::endl;
        outputImageViews = createImageViews(device, dispatchTable, format, outputImages);
        std::cout << "after creating output ImageViews" << std::endl;
        sampler = createSampler(device, dispatchTable);
        std::cout << "after creating sampler" << std::endl;

        VkExtent3D areaImageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1};
        areaImage = createImages(instanceDispatchTable,
                                 device,
                                 dispatchTable,
                                 physicalDevice,
                                 1,
                                 areaImageExtent,
                                 VK_FORMAT_R8G8_UNORM,//TODO search for format and save it
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 areaMemory)[0];
        VkExtent3D searchImageExtent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1};
        searchImage = createImages(instanceDispatchTable,
                                   device,
                                   dispatchTable,
                                   physicalDevice,
                                   1,
                                   searchImageExtent,
                                   VK_FORMAT_R8_UNORM,//TODO search for format and save it
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   searchMemory)[0];

        uploadToImage(instanceDispatchTable,
                       device,
                       dispatchTable,
                       physicalDevice,
                       areaImage,
                       areaImageExtent,
                       AREATEX_SIZE,
                       queue,
                       commandPool,
                       areaTexBytes);

        uploadToImage(instanceDispatchTable,
                       device,
                       dispatchTable,
                       physicalDevice,
                       searchImage,
                       searchImageExtent,
                       SEARCHTEX_SIZE,
                       queue,
                       commandPool,
                       searchTexBytes);

        areaImageView = createImageViews(device, dispatchTable, VK_FORMAT_R8G8_UNORM, std::vector<VkImage>(1,areaImage))[0];
        std::cout << "after creating area ImageView" << std::endl;
        searchImageView = createImageViews(device, dispatchTable, VK_FORMAT_R8_UNORM, std::vector<VkImage>(1,searchImage))[0];
        std::cout << "after creating search ImageView" << std::endl;

        imageSamplerDescriptorSetLayout = createImageSamplerDescriptorSetLayout(device, dispatchTable, 5);
        std::cout << "after creating descriptorSetLayouts" << std::endl;

        VkDescriptorPoolSize imagePoolSize;
        imagePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imagePoolSize.descriptorCount = inputImages.size()*5;

        std::vector<VkDescriptorPoolSize> poolSizes = {imagePoolSize};

        descriptorPool = createDescriptorPool(device, dispatchTable, poolSizes);
        std::cout << "after creating descriptorPool" << std::endl;

        //get config options
        SmaaOptions smaaOptions;
        smaaOptions.threshold           = std::stod(pConfig->getOption("smaaThreshold", "0.05"));
        smaaOptions.maxSearchSteps      = std::stoi(pConfig->getOption("smaaMaxSearchSteps", "32"));
        smaaOptions.maxSearchStepsDiag  = std::stoi(pConfig->getOption("smaaMaxSearchStepsDiag", "16"));
        smaaOptions.cornerRounding      = std::stoi(pConfig->getOption("smaaCornerRounding", "25"));

        auto shaderCode = readFile(smaaEdgeVertexFile);
        createShaderModule(device, dispatchTable, shaderCode, &edgeVertexModule);
        shaderCode = pConfig->getOption("smaaEdgeDetection", "luma") == "color"
            ? readFile(smaaEdgeColorFragmentFile)
            : readFile(smaaEdgeLumaFragmentFile);
        createShaderModule(device, dispatchTable, shaderCode, &edgeFragmentModule);
        shaderCode = readFile(smaaBlendVertexFile);
        createShaderModule(device, dispatchTable, shaderCode, &blendVertexModule);
        shaderCode = readFile(smaaBlendFragmentFile);
        createShaderModule(device, dispatchTable, shaderCode, &blendFragmentModule);
        shaderCode = readFile(smaaNeighborVertexFile);
        createShaderModule(device, dispatchTable, shaderCode, &neighborVertexModule);
        shaderCode = readFile(smaaNeighborFragmentFile);
        createShaderModule(device, dispatchTable, shaderCode, &neignborFragmentModule);

        renderPass      = createRenderPass(device, dispatchTable, format);
        unormRenderPass = createRenderPass(device, dispatchTable, VK_FORMAT_B8G8R8A8_UNORM);

        std::vector<VkDescriptorSetLayout> descriptorSetLayouts = {imageSamplerDescriptorSetLayout};
        pipelineLayout = createGraphicsPipelineLayout(device, dispatchTable, descriptorSetLayouts);

        std::vector<VkSpecializationMapEntry> specMapEntrys(8);
        for(uint32_t i=0;i<specMapEntrys.size();i++)
        {
            specMapEntrys[i].constantID = i;
            specMapEntrys[i].offset = sizeof(float) * i;//TODO not clean to assume that sizeof(int32_t) == sizeof(float)
            specMapEntrys[i].size = sizeof(float);
        }
        smaaOptions.screenWidth = (float) imageExtent.width,
        smaaOptions.screenHeight = (float) imageExtent.height,
        smaaOptions.reverseScreenWidth = 1.0f/imageExtent.width;
        smaaOptions.reverseScreenHeight = 1.0f/imageExtent.height;

        VkSpecializationInfo specializationInfo;
        specializationInfo.mapEntryCount = specMapEntrys.size();
        specializationInfo.pMapEntries = specMapEntrys.data();
        specializationInfo.dataSize = sizeof(smaaOptions);
        specializationInfo.pData = &smaaOptions;

        edgePipeline     = createGraphicsPipeline(device, dispatchTable, edgeVertexModule, &specializationInfo, edgeFragmentModule, &specializationInfo, imageExtent, unormRenderPass, pipelineLayout);
        blendPipeline    = createGraphicsPipeline(device, dispatchTable, blendVertexModule, &specializationInfo, blendFragmentModule, &specializationInfo, imageExtent, unormRenderPass, pipelineLayout);
        neighborPipeline = createGraphicsPipeline(device, dispatchTable, neighborVertexModule, &specializationInfo, neignborFragmentModule, &specializationInfo, imageExtent, renderPass, pipelineLayout);


        std::vector<std::vector<VkImageView>> imageViewsVector = {inputImageViews,
                                                                  edgeImageViews,
                                                                  std::vector<VkImageView>(inputImageViews.size(), areaImageView),
                                                                  std::vector<VkImageView>(inputImageViews.size(), searchImageView),
                                                                  blendImageViews};
        imageDescriptorSets = allocateAndWriteImageSamplerDescriptorSets(device, dispatchTable, descriptorPool, imageSamplerDescriptorSetLayout, sampler, imageViewsVector);

        edgeFramebuffers     = createFramebuffers(device, dispatchTable, unormRenderPass, imageExtent,   edgeImageViews);
        blendFramebuffers    = createFramebuffers(device, dispatchTable, unormRenderPass, imageExtent,  blendImageViews);
        neignborFramebuffers = createFramebuffers(device, dispatchTable, renderPass,      imageExtent, outputImageViews);
    }
    void SmaaEffect::applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer)
    {
        std::cout << "applying smaa effect" << commandBuffer << std::endl;
        //Used to make the Image accessable by the shader
        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext = nullptr;
        memoryBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        memoryBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image = inputImages[imageIndex];
        memoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel = 0;
        memoryBarrier.subresourceRange.levelCount = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount = 1;

        //Reverses the first Barrier
        VkImageMemoryBarrier secondBarrier;
        secondBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        secondBarrier.pNext = nullptr;
        secondBarrier.srcAccessMask =  VK_ACCESS_SHADER_READ_BIT;
        secondBarrier.dstAccessMask =  0;
        secondBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        secondBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        secondBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        secondBarrier.image = inputImages[imageIndex];
        secondBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        secondBarrier.subresourceRange.baseMipLevel = 0;
        secondBarrier.subresourceRange.levelCount = 1;
        secondBarrier.subresourceRange.baseArrayLayer = 0;
        secondBarrier.subresourceRange.layerCount = 1;

        dispatchTable.CmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0, nullptr,0, nullptr,1, &memoryBarrier);
        std::cout << "after the first pipeline barrier" << std::endl;

        VkRenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.pNext = nullptr;
        renderPassBeginInfo.renderPass = unormRenderPass;
        renderPassBeginInfo.framebuffer = edgeFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset = {0,0};
        renderPassBeginInfo.renderArea.extent = imageExtent;
        VkClearValue clearValue = {0.0f, 0.0f, 0.0f, 1.0f};
        renderPassBeginInfo.clearValueCount = 1;
        renderPassBeginInfo.pClearValues = &clearValue;
        //edge renderPass
        std::cout << "before beginn edge renderpass" << std::endl;
        dispatchTable.CmdBeginRenderPass(commandBuffer,&renderPassBeginInfo,VK_SUBPASS_CONTENTS_INLINE);
        std::cout << "after beginn renderpass" << std::endl;

        dispatchTable.CmdBindDescriptorSets(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&(imageDescriptorSets[imageIndex]),0,nullptr);
        std::cout << "after binding image sampler" << std::endl;

        dispatchTable.CmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,edgePipeline);
        std::cout << "after bind pipeliene" << std::endl;

        dispatchTable.CmdDraw(commandBuffer, 3, 1, 0, 0);
        std::cout << "after draw" << std::endl;

        dispatchTable.CmdEndRenderPass(commandBuffer);
        std::cout << "after end renderpass" << std::endl;

        memoryBarrier.image = edgeImages[imageIndex];
        renderPassBeginInfo.framebuffer = blendFramebuffers[imageIndex];
        //blend renderPass
        dispatchTable.CmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0, nullptr,0, nullptr,1, &memoryBarrier);
        std::cout << "after the first pipeline barrier" << std::endl;

        std::cout << "before beginn blend renderpass" << std::endl;
        dispatchTable.CmdBeginRenderPass(commandBuffer,&renderPassBeginInfo,VK_SUBPASS_CONTENTS_INLINE);
        std::cout << "after beginn renderpass" << std::endl;

        dispatchTable.CmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,blendPipeline);
        std::cout << "after bind pipeliene" << std::endl;

        dispatchTable.CmdDraw(commandBuffer, 3, 1, 0, 0);
        std::cout << "after draw" << std::endl;

        dispatchTable.CmdEndRenderPass(commandBuffer);
        std::cout << "after end renderpass" << std::endl;

        memoryBarrier.image = blendImages[imageIndex];
        renderPassBeginInfo.framebuffer = neignborFramebuffers[imageIndex];
        renderPassBeginInfo.renderPass = renderPass;
        //neighbor renderPass
        dispatchTable.CmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0, nullptr,0, nullptr,1, &memoryBarrier);
        std::cout << "after the first pipeline barrier" << std::endl;

        std::cout << "before beginn neighbor renderpass" << std::endl;
        dispatchTable.CmdBeginRenderPass(commandBuffer,&renderPassBeginInfo,VK_SUBPASS_CONTENTS_INLINE);
        std::cout << "after beginn renderpass" << std::endl;

        dispatchTable.CmdBindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,neighborPipeline);
        std::cout << "after bind pipeliene" << std::endl;

        dispatchTable.CmdDraw(commandBuffer, 3, 1, 0, 0);
        std::cout << "after draw" << std::endl;

        dispatchTable.CmdEndRenderPass(commandBuffer);
        std::cout << "after end renderpass" << std::endl;

        dispatchTable.CmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0, nullptr,0, nullptr,1, &secondBarrier);
        std::cout << "after the second pipeline barrier" << std::endl;

    }
    SmaaEffect::~SmaaEffect()
    {
        std::cout << "destroying smaa effect " << this << std::endl;
        dispatchTable.DestroyPipeline(device, edgePipeline,     nullptr);
        dispatchTable.DestroyPipeline(device, blendPipeline,    nullptr);
        dispatchTable.DestroyPipeline(device, neighborPipeline, nullptr);

        dispatchTable.DestroyPipelineLayout(device,pipelineLayout,nullptr);
        dispatchTable.DestroyRenderPass(device,renderPass,nullptr);
        dispatchTable.DestroyRenderPass(device,unormRenderPass,nullptr);
        dispatchTable.DestroyDescriptorSetLayout(device,imageSamplerDescriptorSetLayout,nullptr);

        dispatchTable.DestroyShaderModule(device, edgeVertexModule,       nullptr);
        dispatchTable.DestroyShaderModule(device, edgeFragmentModule,     nullptr);
        dispatchTable.DestroyShaderModule(device, blendVertexModule,      nullptr);
        dispatchTable.DestroyShaderModule(device, blendFragmentModule,    nullptr);
        dispatchTable.DestroyShaderModule(device, neighborVertexModule,   nullptr);
        dispatchTable.DestroyShaderModule(device, neignborFragmentModule, nullptr);

        dispatchTable.DestroyDescriptorPool(device,descriptorPool,nullptr);
        dispatchTable.FreeMemory(device,imageMemory,nullptr);
        dispatchTable.FreeMemory(device,areaMemory,nullptr);
        dispatchTable.FreeMemory(device,searchMemory,nullptr);
        for(unsigned int i=0;i<edgeFramebuffers.size();i++)
        {
            dispatchTable.DestroyFramebuffer(device,edgeFramebuffers[i],nullptr);
            dispatchTable.DestroyFramebuffer(device,blendFramebuffers[i],nullptr);
            dispatchTable.DestroyFramebuffer(device,neignborFramebuffers[i],nullptr);
            dispatchTable.DestroyImageView(device,inputImageViews[i],nullptr);
            dispatchTable.DestroyImageView(device,edgeImageViews[i],nullptr);
            dispatchTable.DestroyImageView(device,blendImageViews[i],nullptr);
            dispatchTable.DestroyImageView(device,outputImageViews[i],nullptr);
            dispatchTable.DestroyImage(device,edgeImages[i],nullptr);
            dispatchTable.DestroyImage(device,blendImages[i],nullptr);
            std::cout << "after DestroyImageView" << std::endl;
        }
        dispatchTable.DestroyImageView(device,areaImageView,nullptr);
        dispatchTable.DestroyImage(device,areaImage,nullptr);
        dispatchTable.DestroyImageView(device,searchImageView,nullptr);
        dispatchTable.DestroyImage(device,searchImage,nullptr);

        dispatchTable.DestroySampler(device,sampler,nullptr);
    }
}
