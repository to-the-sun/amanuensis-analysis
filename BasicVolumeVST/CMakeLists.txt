# Basic Volume VST CMakeLists.txt

cmake_minimum_required(VERSION 3.22)

project(BASIC_VOLUME_VST VERSION 0.1.0)

# Include JUCE
add_subdirectory(../juce BasicVolumeVST/juce_build)

juce_add_plugin(BasicVolumeVST
    PLUGIN_MANUFACTURER_CODE User
    PLUGIN_CODE Bvol
    FORMATS VST3 Standalone
    PRODUCT_NAME "Basic Volume VST")

target_sources(BasicVolumeVST
    PRIVATE
        PluginEditor.cpp
        PluginProcessor.cpp)

target_compile_definitions(BasicVolumeVST
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(BasicVolumeVST
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
