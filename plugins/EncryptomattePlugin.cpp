// Copyright (c) 2021 The Foundry Visionmongers Ltd. All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright notice,
//       this list of conditions and the following disclaimer in the documentation
//       and/or other materials provided with the distribution.
//     * Neither the name of Foundry nor the names of its
//       contributors may be used to endorse or promote products derived from this
//       software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// This work is a C++ implementation compatible with version 1.2.8 of the Python-based Nuke gizmo
// authored at Psyop by Jonah Friedman and Andy Jones (see https://github.com/Psyop/Cryptomatte).

#include "EncryptomattePlugin.h"

#include "CryptomatteLayerDialog.moc.h"
#include "CryptomatteLayerKnob.h"
#include "CryptomatteManifest.h"
#include "CryptomatteUtils.h"
#include "EncryptomatteUtils.h"

#include "DDImage/Knobs.h"
#include "DDImage/LayerI.h"
#include "DDImage/Row.h"

#include <iomanip>

namespace Foundry {
  namespace NukePlugins {

    using namespace DD::Image;
    using namespace Cryptomatte;
    using namespace Encryptomatte;

    enum class MatteNameType : uint8_t {
      Auto,
      Manual
    };

    enum class MergeOperation : uint8_t {
      Over,
      Under
    };

    // Knobs
    // Note: to avoid knob names mismatch with the original gizmo,
    // don't change the name of knobs followed by "Name from gizmo" comment
    const char* kMatteNameKnob = "matteName";  // Name from gizmo
    const char* kPreviousMatteNameKnob = "previousMatteName";
    const char* kCryptoLayerKnob = "cryptoLayer";  // Name from gizmo
    const char* kMatteNameTypeKnob = "matteNameType";
    const char* kMergeOperationKnob = "mergeOperation";
    const char* kExportWriteKnob = "exportWrite";
    const std::string kExportWriteScript = std::string("\
encryptomatteNode = nuke.thisNode() \n\
writeNode = nuke.nodes.Write() \n\
writeNode.setInput(0,encryptomatteNode) \n\
nextNode = writeNode.dependent(nuke.INPUTS) \n\
while(nextNode): \n\
  inputNode = nextNode.pop() \n\
  inputs = inputNode.inputs() \n\
  for i in range(0,inputs-1): \n\
    if(inputNode.input(i) == writeNode): \n\
      inputNode.setInput(i,encryptomatteNode) \n\
x = encryptomatteNode.xpos() \n\
y = encryptomatteNode.ypos() \n\
w = encryptomatteNode.screenWidth() \n\
h = encryptomatteNode.screenHeight() \n\
m = int(x + w/2) \n\
writeNode.setXYpos(int(m + w), int(y + w/2)) \n\
writeNode['channels'].setValue('all') \n\
writeNode['file_type'].setValue('exr') \n\
writeNode['datatype'].setValue('32 bit float') \n\
writeNode['metadata'].setValue('all metadata') \n\
\n");

    const char* const kMatteNameTypeOptions[] = {"auto", "manual", nullptr};
    const char* const kMergeOperationOptions[] = {"over", "under", nullptr};

    EncryptomattePlugin::EncryptomattePlugin(Node* node)
    : Iop(node)
    , _cryptoLayerDepth(kDefaultCryptoLayerDepth)
    {
      inputs(2);
    }

    const char* EncryptomattePlugin::Class() const
    {
      return kDescription.name;
    }

    const char* EncryptomattePlugin::node_help() const
    {
      return "<p>Encryptomatte: Composites new mattes to be added to a set of "
             "Cryptomatte-encoded mattes.</p>";
    }

    void EncryptomattePlugin::setCryptomatteSublayers()
    {
      if (_cryptoLayerName.empty()) {
        _cryptomatteSublayers.clear();
        return;
      }

      _cryptomatteSublayers.resize(_cryptoLayerDepth);

      for (size_t i = 0; i < _cryptoLayerDepth; ++i) {
        std::stringstream cryptomatteStream;
        cryptomatteStream << std::setw(2) << std::setfill('0') << i;
        std::string cryptomatteSublayerName = std::string(_cryptoLayerName) + cryptomatteStream.str();
        // Create the layers and channels when set without GUI if they do not
        // exist.
        for (const std::string& channelName : kChannelNames) {
          const std::string layerChannelName = cryptomatteSublayerName + "." + channelName;
          getChannel(layerChannelName.c_str());
        }
        LayerI* cryptomatteSublayer = GetLayer(cryptomatteSublayerName);
        _cryptomatteSublayers[i] = cryptomatteSublayer;
      }
    }

    void EncryptomattePlugin::checkLayerInitialised()
    {
      if (_cryptoLayerName.empty()) {
        _isOwnLayer = false;
        _isOwnLayerInitialised = false;
        _previousCryptoLayerName = _cryptoLayerName;
        return;
      }

      const std::string cryptoLayerName = _cryptoLayerName;
      if (_previousCryptoLayerName != cryptoLayerName) {
        const auto cryptomatteObjects = GetAvailableCryptomatteObjects(this);
        _isOwnLayer = true;
        _isOwnLayerInitialised = false;
        for (const auto& cryptomatteObject : cryptomatteObjects) {
          if (cryptomatteObject.name == cryptoLayerName) {
            _isOwnLayer = false;
            break;
          }
        }
        _previousCryptoLayerName = cryptoLayerName;
      }
      else {
        _isOwnLayerInitialised = true;
      }
    }

    std::string EncryptomattePlugin::getMatteName() const
    {
      const auto& outputContext = uiContext();
      const auto matteInput = op_cast<Iop*>(node_input(1, Op::EXECUTABLE_INPUT, &outputContext));
      const bool matteInputConnected = (matteInput != nullptr && matteInput != default_input(1));
      if (matteInputConnected) {
        return matteInput->node_name();
      }

      return std::string();
    }

    void EncryptomattePlugin::_validate(bool forReal)
    {
      copy_info();

      // It is currently the union of input 0 and the matte.
      // The matte input has to be validated explicitly to get the matte
      // displayed for some reason. This could be investigated why given that
      // merge_info also calls validate internally, although slightly
      // differently.
      input(1)->validate(forReal);
      merge_info(1, Chan_Alpha);

      ChannelSet cryptomatteChannels = Mask_None;
      setCryptomatteSublayers();
      for (auto const cryptomatteSublayer : _cryptomatteSublayers) {
        cryptomatteChannels += cryptomatteSublayer->getChannelSet();
      }

      info_.turn_on(cryptomatteChannels);
      set_out_channels(cryptomatteChannels);

      checkLayerInitialised();

      _isOver = static_cast<MergeOperation>(_mergeOperationIndex) == MergeOperation::Over;

      const bool isMatteNameAuto = static_cast<MatteNameType>(_matteNameTypeIndex) == MatteNameType::Auto;
      _matteId = GetMurmurHashValueFloat(isMatteNameAuto ? getMatteName() : _matteName);
    }

    void EncryptomattePlugin::in_channels(int input, ChannelSet& mask) const
    {
      // Out channels will be completely overwritten.
      // We don't need anything from input on them.
      mask -= out_channels();

      if (input == 0) {
        for (auto const cryptomatteSublayer : _cryptomatteSublayers) {
          mask += cryptomatteSublayer->getChannelSet();
        }
      }
      else {
        // Matte input channel
        mask += Chan_Alpha;
      }
    }

    const char* EncryptomattePlugin::input_label(int input, char* buffer) const
    {
      switch (input) {
        case 0:
          return nullptr;
        case 1:
          return "Matte";
        default:
          return nullptr;
      }
    }

    void EncryptomattePlugin::knobs(Knob_Callback callback)
    {
      Enumeration_knob(callback, &_matteNameTypeIndex, kMatteNameTypeOptions, kMatteNameTypeKnob, "Matte Name");
      Tooltip(callback, "Whether the matte name is set automatically by using "
                        "the name of the matte source node, or set manually by "
                        "entering a name into the matteName text field.");

      String_knob(callback, &_matteName, kMatteNameKnob, "");
      SetFlags(callback, Knob::DISABLED);
      Tooltip(callback, "Enter the descriptive name of the matte. If the name "
        "is empty, the node does nothing.");

      // Used for restoring the previous value when the matte name is removed in manual mode
      String_knob(callback, &_previousMatteName, kPreviousMatteNameKnob);
      SetFlags(callback, Knob::INVISIBLE);

      Enumeration_knob(callback, &_mergeOperationIndex, kMergeOperationOptions, kMergeOperationKnob, "Merge Operation");
      Tooltip(callback, "Choose a compositing mode to control how your new "
                        "matte will be merged with the existing set.\n\nThe new"
                        " matte will be treated like the A input, while the "
                        "existing mattes will be treated like the B input. The "
                        "combined result of A and B will be placed over the "
                        "background matte.");

      Knob* cryptomatteLayerKnob = CustomKnob2(CryptomatteLayer_Knob, callback, &_cryptoLayerName, kCryptoLayerKnob, "Cryptomatte Layer");
      // Make the crptomatte layer knob accessible through Python
      if (callback.makeKnobs()) {
        callback(PLUGIN_PYTHON_KNOB, Custom, cryptomatteLayerKnob, nullptr, nullptr, nullptr);
      }
      Tooltip(callback, "Choose a Cryptomatte layer in which to put the matte, "
        "or create a new Cryptomatte layer by selecting the <i>new</i> option. "
        "If no layer is selected, the knob will say <i>none</i> and the matte "
        "will not be placed in any layer.");

      Divider(callback, "Export");
      PyScript_knob(callback, kExportWriteScript.c_str(), kExportWriteKnob, "Export Write");
      Tooltip(callback, "Press this to create a Write node with Cryptomatte compatible export settings.");
      Newline(callback);
    }

    bool EncryptomattePlugin::updateUI(const OutputContext& context)
    {
      // Auto-labeling
      if (isMatteNameManual()) {
        return true;
      }

      auto matteNameKnob = knob(kMatteNameKnob);
      if (matteNameKnob == nullptr) {
        return true;
      }

      const std::string matteName = getMatteName();
      matteNameKnob->set_text(matteName.c_str());
      return true;
    }

    int EncryptomattePlugin::knob_changed(Knob* k)
    {
      if (k->is(kMatteNameTypeKnob)) {
        auto matteNameKnob = knob(kMatteNameKnob);
        if (matteNameKnob != nullptr) {
          matteNameKnob->enable(isMatteNameManual());
        }
        return 1;
      }
      if (k->is(kMatteNameKnob)) {
        auto matteNameKnob = knob(kMatteNameKnob);
        auto previousMatteNameKnob = knob(kPreviousMatteNameKnob);
        if (matteNameKnob != nullptr && previousMatteNameKnob) {
          const char* matteName = matteNameKnob->get_text();
          if (isMatteNameManual() && (matteName == nullptr || *matteName == '\0')) {
            matteNameKnob->set_text(previousMatteNameKnob->get_text());
          } else {
            previousMatteNameKnob->set_text(matteName);
          }
        }
        return 1;
      }
      return 0;
    }

    void EncryptomattePlugin::append(Hash& hash)
    {
      hash.append(getMatteName());
    }

    const MetaData::Bundle& EncryptomattePlugin::_fetchMetaData(const char* searchKey)
    {
      _meta = Iop::_fetchMetaData(nullptr);

      if (_cryptoLayerName.empty() || _matteName.empty()) {
        return _meta;
      }

      const std::string layerHexIdentifier = GetMurmurHashValueHex(_cryptoLayerName.c_str());
      // Create the common cryptomatte/aaaaaaa alike prefix. The hex is only 7
      // digits here for cryptomatte layers as opposed to 8 for objects. This
      // is driven by the cryptomatte specification.
      const std::string prefix = std::string("cryptomatte/") + layerHexIdentifier.substr(0, layerHexIdentifier.size() - 1);

      std::string manifestJson;
      for (const auto& metaItem : _meta) {
        const std::string& metaKey = metaItem.first;
        const std::string manifestKey = prefix + "/manifest";
        if (metaKey == manifestKey) {
          manifestJson = MetaData::getPropertyString(metaItem.second, 0);
          break;
        }
      }

      // Add the basic cryptomatte metadata for the new layer. One matte id per
      // new layer. Map is used to avoid code duplication and use one iteration
      // with one setData call.
      Manifest manifest(manifestJson);
      manifest.insert(_matteName, GetMurmurHashValueFloat(_matteName));
      manifestJson = manifest.toJson();
      const std::map<std::string, std::string> keyValueMap = {
        {"name", _cryptoLayerName},
        {"conversion", kSupportedMetadataConversion},
        {"hash", kSupportedMetadataHash},
        {"manifest", manifest.toJson()}};

      for (auto const& keyValue : keyValueMap) {
        const std::string key = prefix + "/" + keyValue.first;
        _meta.setData(key.c_str(), keyValue.second);
      }

      // Return the modified metadata
      return _meta;
    }

    std::vector<const float*> EncryptomattePlugin::createInputRowData(const Row& in, int x, int r)
    {
      const size_t width = static_cast<size_t>(r - x);
      const size_t channelNamesSize = kChannelNames.size();
      const size_t numberOfChannels = _cryptoLayerDepth * channelNamesSize;
      std::vector<const float*> inputRowData(numberOfChannels);

      // TODO: can we use "mask" for this instead of the layers?
      const size_t layerSize = _cryptomatteSublayers.size();
      // TODO: Investigate whether we can avoid getting into this loop if the
      // first input is not connected. Could this cause issues, like a crash,
      // other than performance?
      // Set up the input channel vectors (copies) for all channels of all layers.
      // e.g. Layer00.{r,g,b,a}, Layer01.{r,g,b,a}, Layer02.{r,g,b,a}
      for (int layerIndex = 0; layerIndex < layerSize; ++layerIndex) {
        const std::string layerName = _cryptomatteSublayers[layerIndex]->name();
        for (size_t channelNamesIndex = 0; channelNamesIndex < channelNamesSize; ++channelNamesIndex) {
          const std::string channelName = layerName + std::string(".") + kChannelNames[channelNamesIndex];
          const Channel channel = getChannel(channelName.c_str());
          const size_t channelIndex = layerIndex * channelNamesSize + channelNamesIndex;
          inputRowData[channelIndex] = in[channel] + x;
        }
      }
      return inputRowData;
    }

    std::vector<float*> EncryptomattePlugin::createOutputRowData(Row& out, int x, int r)
    {
      const size_t channelNamesSize = kChannelNames.size();
      const size_t numberOfChannels = _cryptoLayerDepth * channelNamesSize;
      std::vector<float*> outputRowData(numberOfChannels);

      // TODO: can we use "mask" for this instead of the layers?
      const size_t layerSize = _cryptomatteSublayers.size();
      // Set up the output writable pointers for all channels of all layers.
      for (int layerIndex = 0; layerIndex < layerSize; ++layerIndex) {
        const std::string layerName = _cryptomatteSublayers[layerIndex]->name();
        for (size_t channelNamesIndex = 0; channelNamesIndex < channelNamesSize; ++channelNamesIndex) {
          const std::string channelName = layerName + std::string(".") + kChannelNames[channelNamesIndex];
          const Channel channel = getChannel(channelName.c_str());
          const size_t channelIndex = layerIndex * channelNamesSize + channelNamesIndex;
          outputRowData[channelIndex] = out.writable(channel) + x;
        }
      }
      return outputRowData;
    }

    void EncryptomattePlugin::engine(int y, int x, int r, ChannelMask channels, Row& row)
    {
      ChannelSet engineChans = out_channels();
      engineChans &= (channels);
      if (!engineChans) {
        input0().get(y, x, r, channels, row);
        return;
      }

      ChannelSet myRequestChans = channels;
      in_channels(0, myRequestChans);

      input0().get(y, x, r, myRequestChans, row);
      pixel_engine(row, y, x, r, engineChans, row);
    }

    void EncryptomattePlugin::pixel_engine(const Row& in, int y, int x, int r, ChannelMask mask, Row& out)
    {
      if (r < x) {
        // Return, if we are in the degenerate case
        return;
      }

      if (_cryptoLayerName.empty()) {
        return;
      }

      Row matteRow(x, r);
      input1().get(y, x, r, Chan_Alpha, matteRow);
      const float* inptr = matteRow[Chan_Alpha] + x;

      const std::vector<const float*> inputRowData = createInputRowData(in, x, r);

      const size_t width = static_cast<size_t>(r - x);
      std::vector<CoverageIdsMap> coverageIdPairsRow(width);
      // Iterate through each pixel by their index in the row.
      for (size_t rowIndex = 0; rowIndex < width; ++rowIndex) {
        const float matteAlpha = inptr[rowIndex];
        const bool initialise = (_isOwnLayer && !_isOwnLayerInitialised) || (inputRowData[0][rowIndex] == kFillingObjectId && inputRowData[1][rowIndex] == kCoverageEmpty);

        if (initialise) {
          coverageIdPairsRow[rowIndex] = InitialisePixel(_matteId, matteAlpha, rowIndex);
        }

        if (coverageIdPairsRow[rowIndex].empty()) {
          coverageIdPairsRow[rowIndex] = CopyPixelOutsideMatte(inputRowData, rowIndex, matteAlpha);
        }

        if (coverageIdPairsRow[rowIndex].empty()) {
          coverageIdPairsRow[rowIndex] = CreateCoverageIdPairs(inputRowData, rowIndex, _matteId, matteAlpha, _isOver);
        }
      }

      const std::vector<float*> outputRowData = createOutputRowData(out, x, r);
      for (size_t rowIndex = 0; rowIndex < width; ++rowIndex) {
        WriteOutputRowData(coverageIdPairsRow[rowIndex], outputRowData, rowIndex);
      }
    }

    bool EncryptomattePlugin::isMatteNameManual() const
    {
      const auto matteNameTypeKnob = knob(kMatteNameTypeKnob);
      if (matteNameTypeKnob == nullptr) {
        return static_cast<MatteNameType>(_matteNameTypeIndex) == MatteNameType::Manual;
      }

      // Disable the matte name knob when the matte name type is manual; otherwise enable it.
      return static_cast<MatteNameType>(static_cast<int>(matteNameTypeKnob->get_value())) == MatteNameType::Manual;
    }

    static Iop* build(Node* node) { return new EncryptomattePlugin(node); }
    const Iop::Description EncryptomattePlugin::kDescription("Encryptomatte", nullptr, build);
  }
}
