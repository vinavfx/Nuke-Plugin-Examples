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

#include "CryptomattePlugin.h"
#include "CryptomatteManifest.h"
#include "CryptomatteMatteList.h"
#include "Build/fnBuild.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Enumeration_KnobI.h"
#include <iomanip>

namespace {
  const char* const kManifestSourceOptions[] = { "Metadata", "Sidecar", nullptr };
  constexpr char* kInsideManifestSourceProceduralModeficatorKnob = "insideManifestSourceProceduralModeficatorKnob";

  // The helper type for distinguishing the procedural modification of the knob from the real user input.
  // The class operates via a dedicated numeric knob. Zero value of the knob means the procedural
  // modification session is closed. A non-zero value of the counter knob determines an enclosure level
  // of the open session.
  // This is implemented through the memory management mechanism. When an instance of the class is created,
  // the value of the counter knob is increased. Similarly, the value of the counter knob is decreased,
  // when the instance gets destructed.
  class KnobProceduralModeficationSession
  {
    DD::Image::Knob* _couterKnob;
  public:
    KnobProceduralModeficationSession(DD::Image::Op* op, char* couterKnobName)
    {
      _couterKnob = op->knob(couterKnobName);
      if (_couterKnob != nullptr) {
        double counter = _couterKnob->get_value();
        ++counter;
        _couterKnob->set_value(counter);
      }
    }
    ~KnobProceduralModeficationSession()
    {
      if (_couterKnob != nullptr) {
        double counter = _couterKnob->get_value();
        --counter;
        _couterKnob->set_value(counter);
        mFnAssert(counter >= 0);
      }
    }
    static bool AtProceduralModefication(DD::Image::Op* op, char* couterKnobName)
    {
      if (auto couterKnob = op->knob(couterKnobName)) {
        return couterKnob->get_value() > 0;
      }
      return false;
    }
  };
}

namespace Foundry {
  namespace NukePlugins {

    using namespace DD::Image;
    using namespace Cryptomatte;

    enum ManifestSource : size_t {
      eEmbeddedManifest = 0,
      eSidecarManifest
    };

    // Knobs
    // Note: to avoid knob names mismatch with the original gizmo,
    // don't change the name of knobs followed by "Name from gizmo" comment
    const char* CryptomattePlugin::kCryptoLayerChoiceKnob = "cryptoLayerChoice";  // Name from gizmo
    const char* CryptomattePlugin::kManifestSourceKnob = "manifestSource";
    const char* CryptomattePlugin::kSidecarFilepathKnob = "sidecarFilepath";
    const char* CryptomattePlugin::kMatteOutputKnob = "matteOutput";              // Name from gizmo
    const char* CryptomattePlugin::kUnpremultiply = "unpremultiply";              // Name from gizmo
    const char* CryptomattePlugin::kRemoveChannelsKnob = "removeChannels";
    const char* CryptomattePlugin::kManifestSourceModifiedKnob = "manifestSourceModified";
    const char* CryptomattePlugin::kLastSelectedCryptoLayerNameKnob = "lastSelectedCryptoLayerName";

    CryptomattePlugin::CryptomattePlugin(Node* node)
    : PixelIop(node)
    , _picker(this)
    { }

    const char* CryptomattePlugin::Class() const
    {
      return kDescription.name;
    }

    const char* CryptomattePlugin::node_help() const
    {
      return "<p>Cryptomatte: Generates a matte that allows modification of parts of a 3D scene in a 2D render.</p>"
        "<p>To use, place the node on a stream that contains Cryptomatte layers (e.g. provided by an exr file, "
        "with Cryptomatte AOVs included). Then tick the Picker Add knob and use " FN_CTRLKEYNAME "+click or "
        FN_CTRLKEYNAME "+Shift+drag to select ID mattes from the viewer. These are then used to generate a matte "
        "in the target channel, specified by the Matte Output knob. Once an ID matte is selected, its name gets "
        "added to the Matte List knob. Similarly to Picker Add, you can use the Picker Remove knob to remove "
        "selected ID mattes. You can also manipulate selected ID mattes by modifying the text in Matte List.</p>"
        "<p>The Cryptomatte specifications were created at Psyop by Jonah Friedman and Andy Jones. They define "
        "a fully automatic method for creating and encoding ID mattes. 3D scenes consist of an organised hierarchy "
        "of objects and relationships, and when 2D images are rendered, that information is not preserved. "
        "ID mattes attempt to preserve organisational information by establishing a correspondence between items in "
        "the 3D scene and particular pixels in the 2D image plane.</p>";
    }

    void CryptomattePlugin::_validate(bool forReal)
    {
      copy_info();

      // Remove unused channels if requested.
      if (_removeChannels) {
        info_.channels() &= Mask_RGBA;
      }

      // Determine out channels.
      ChannelSet outChannels = _matteOutChannel;
      // If Preview Mode is enabled, add RGB to out channels.
      if (_picker.previewIsEnabled()) {
        outChannels += Mask_RGB;
      }
      info_.turn_on(outChannels);
      set_out_channels(outChannels);

      // Fetch available cryptomatte objects from the input 0.
      const auto availableObjects = GetAvailableCryptomatteObjects(this);

      // Pick user-selected cryptomatte among the available items.
      // List of available cryptomattes may be empty, when the input 0 isn't present.
      if (availableObjects.empty()) {
        _selectedCryptomatte = CryptomatteObject();
      }
      else {
        // updateUI() tracks cryptomatte data change on the input and updates the selected
        // index accordingly. When the plugin's UI isn't present on the Properties pane,
        // cryptomatte data change may not be detected, therefore the selected index could
        // be invalid. This could make the selected index being out of bounds of actual
        // cryptomatte objects. Below we use the value of the select index if the index is
        // in bounds; otherwise 0.
        const auto safeSelectedIndex =
          (_selectedCryptomatteIndex >= availableObjects.size()) ? 0 : static_cast<size_t>(_selectedCryptomatteIndex);
        _selectedCryptomatte = availableObjects[safeSelectedIndex];

        // Restore the last selected cryptomatte layer name if found.
        if (_selectedCryptomatte.name != _lastSelectedCryptoLayerName) {
          for (const auto& cryptoObject : availableObjects) {
            if (_lastSelectedCryptoLayerName == cryptoObject.name) {
              _selectedCryptomatte = cryptoObject;
              break;
            }
          }
        }

        HandleCryptomatteObjectErrors(_selectedCryptomatte, this);
      }

      // Get Manifest to allow pattern matching when using wildcards expressions.
      const auto manifest = getManifestFromSelectedSource(_selectedCryptomatte);

      // Resolve IDs from Matte List for further use in pixel_engine().
      _resolvedIDs = _picker.getResolvedIDs(manifest);
    }

    void CryptomattePlugin::in_channels(int input, ChannelSet& mask) const
    {
      // Out channels will be completely overwritten.
      // We don't need anything from input on them.
      mask -= out_channels();

      // Add layers from user-selected cryptomatte to input channels, if any exists
      for (const auto& layer : _selectedCryptomatte.layers) {
        mask += layer;
      }

      if (_unpremultiplyEnabled) {
        mask += Chan_Alpha;
      }
    }

    void CryptomattePlugin::knobs(Knob_Callback callback)
    {
      Enumeration_knob(callback, &_selectedCryptomatteIndex, _cryptoLayersMenu, kCryptoLayerChoiceKnob, "Layer Selection");
      SetFlags(callback, Knob::SAVE_MENU | Knob::EXPAND_TO_CONTENTS);
      Tooltip(callback, "Choose which Cryptomatte layer to generate the output matte from.");
      Newline(callback);

      Enumeration_knob(callback, &_manifestSourceIndex, kManifestSourceOptions, kManifestSourceKnob, "Manifest Source");
      Tooltip(callback, "Choose the source of the manifest for the selected Cryptomatte layer."
        "<p>Note: the value will be reset to the default when the Layer Selection knob is changed.</p>");

      File_knob(callback, &_sidecarFilePath, kSidecarFilepathKnob, "");
      ClearFlags(callback, Knob::STARTLINE);
      Tooltip(callback, "When the Sidecar option is selected as a Manifest Source, use this "
        "knob to set the path to the sidecar file."
        "<p>Note: unless any modification to this knob has been done manually, "
        "the value will be reset to the default when the Layer Selection knob is changed.</p>");

      Divider(callback);

      _picker.addKnobs(callback);

      Divider(callback);

      Channel_knob(callback, &_matteOutChannel, 1, kMatteOutputKnob, "Matte Output");
      Tooltip(callback, "Set the channel the matte will be written to.");

      Bool_knob(callback, &_unpremultiplyEnabled, kUnpremultiply, "Unpremultiply");
      Tooltip(callback, "Unpremultiply the output matte by the input alpha channel.");
      Newline(callback);

      Bool_knob(callback, &_removeChannels, kRemoveChannelsKnob, "Remove Channels");
      Tooltip(callback, "Removes all the channels for the output, except for RGBA channels and the matte output channel.");

      // This invisible knob is used as a state flag, that determines if manifest source knobs
      // have been modified by the user.
      Bool_knob(callback, &_manifestSourceModified, kManifestSourceModifiedKnob);
      SetFlags(callback, Knob::INVISIBLE);

      // This invisible knob is used for preserving the name of last user-selected cryptomatte.
      // When the input is changed, a Cryptomatte object with the same name is selected by default,
      // if any exists upstream.
      String_knob(callback, &_lastSelectedCryptoLayerName, kLastSelectedCryptoLayerNameKnob);
      SetFlags(callback, Knob::INVISIBLE | Knob::NO_UNDO);

      Int_knob(callback, &_insideManifestSourceProceduralModeficator, kInsideManifestSourceProceduralModeficatorKnob);
      SetFlags(callback, Knob::INVISIBLE | Knob::NO_UNDO | Knob::DO_NOT_WRITE);

      addObsoleteKnobs(callback);
    }

    int CryptomattePlugin::knob_changed(Knob* k)
    {
      if (k == &Knob::showPanel) {
        const auto cryptomatteContext = loadCryptomatteContext();
        // Update the layer selection knob with list of available cryptomattes
        updateCryptoLayerChoiceKnob(cryptomatteContext);
        // Update manifest source knob and sidecar file knob
        updateManifestSourceKnobs(cryptomatteContext);
        return 1;
      }
      if (k->is(kCryptoLayerChoiceKnob)) {
        const auto cryptomatteContext = loadCryptomatteContext();
        // Update manifest source knob and sidecar file knob
        updateManifestSourceKnobs(cryptomatteContext);
        // Save last user-selected Crypto Layer
        saveLastSelectedCryptoLayerName(cryptomatteContext);
        return 1;
      }
      if (k->is(kManifestSourceKnob)) {
        const auto cryptomatteContext = loadCryptomatteContext();
        checkManifestSourceModified(cryptomatteContext);
        updateSidecarFilenameKnobEnabled();
        return 1;
      }
      if (k->is(kSidecarFilepathKnob)) {
        const auto cryptomatteContext = loadCryptomatteContext();
        checkManifestSourceModified(cryptomatteContext);
        return 1;
      }
      if (_picker.knob_changed(k)) {
        return 1;
      }
      return 0;
    }

    bool CryptomattePlugin::updateUI(const OutputContext& context)
    {
      // Fetch available Cryptomatte objects from the metadata and update the UI
      // in regards to them, if the UI is outdated.
      const auto cryptomatteContext = loadCryptomatteContext();
      if (cryptomatteContext.cryptoLayersUiInvalid) {
        updateCryptoLayerChoiceKnob(cryptomatteContext);
      }
      if (cryptomatteContext.sidecarFilenameUiInvalid) {
        updateManifestSourceKnobs(cryptomatteContext);
      }
      return true;
    }

    void CryptomattePlugin::pixel_engine(const Row& in, int y, int x, int r, ChannelMask mask, Row& out)
    {
      if (r < x) {
        // Return, if we are in the degenerate case
        return;
      }

      // If for some reason the selected cryptomatte is invalid (e.g. the Input doesn't exist),
      // just clear the Matte Out Channel. If the Preview Mode is enabled, clear RGB as well.
      if (_selectedCryptomatte.layers.empty()) {
        ChannelSet outChannels(_matteOutChannel);
        if (_picker.previewIsEnabled()) {
          outChannels += Mask_RGB;
        }
        out.erase(outChannels);
        return;
      }

      // Render the preview and matte channel
      _picker.renderPreview(this, in, y, x, r, _selectedCryptomatte.layers,
        _resolvedIDs, &_matteOutChannel, _unpremultiplyEnabled, out);
    }

    CryptomatteObject CryptomattePlugin::getSelectedCryptomatteObject() const
    {
      const auto cryptoLayerChoiceKnob = knob(kCryptoLayerChoiceKnob);
      if (cryptoLayerChoiceKnob == nullptr) {
        return CryptomatteObject();
      }

      const auto availableCryptomattes = GetAvailableCryptomatteObjects(this);

      // Get the index of selected cryptomatte and make sure the value is within
      // the range of available cryptomattes.
      const auto selectedCryptomatteIndex = static_cast<size_t>(cryptoLayerChoiceKnob->get_value());
      if (selectedCryptomatteIndex >= availableCryptomattes.size()) {
        return CryptomatteObject();
      }

      return availableCryptomattes[selectedCryptomatteIndex];
    }

    PickingInterface::PixelRowFetcher CryptomattePlugin::getCryptomattePixelRowFetcher() const
    {
      const auto& outputContext = uiContext();
      auto input = op_cast<Iop*>(node_input(0, Op::EXECUTABLE_INPUT, &outputContext));
      if (input == nullptr) {
        return[](int y, int x, int r, ChannelSet m, Row& row) { };
      }

      input->validate();
      input->request(input->info().box(), input->info().channels(), 0);

      return [input](int y, int x, int r, ChannelSet m, Row& row) { input->get(y, x, r, m, row); };
    }

    CryptomattePlugin::CryptomatteContext CryptomattePlugin::loadCryptomatteContext() const
    {
      CryptomatteContext cryptomatteContext;
      const auto layerChoiceKnob = knob(kCryptoLayerChoiceKnob);
      const auto sidecarFilepathKnob = knob(kSidecarFilepathKnob);
      const auto selectedCryptoLayerNameKnob = knob(kLastSelectedCryptoLayerNameKnob);
      if (layerChoiceKnob == nullptr || sidecarFilepathKnob == nullptr ||
        selectedCryptoLayerNameKnob == nullptr) {
        return cryptomatteContext;
      }
      const Enumeration_KnobI* layerChoiceEnumerationKnob = layerChoiceKnob->enumerationKnob();
      if (layerChoiceEnumerationKnob == nullptr) {
        return cryptomatteContext;
      }
      // Fetch available Cryptomatte objects from the input 0
      cryptomatteContext.objects = GetAvailableCryptomatteObjects(this);
      // Determine if Cryptomatte objects have been changed since the last update
      // by comparing those against items from the Layer Selection.
      auto cryptoLayerMenuItems = layerChoiceEnumerationKnob->menu();
      cryptomatteContext.cryptoLayersUiInvalid = cryptoLayerMenuItems.size() != cryptomatteContext.objects.size();
      for (size_t i = 0; !cryptomatteContext.cryptoLayersUiInvalid && i < cryptoLayerMenuItems.size(); ++i) {
        cryptomatteContext.cryptoLayersUiInvalid = cryptoLayerMenuItems[i] != cryptomatteContext.objects[i].name;
      }
      if (!cryptomatteContext.cryptoLayersUiInvalid) {
        // If the UI is up to date, get the selected index from the corresponding knob 
        const size_t lastIndex = static_cast<size_t>(layerChoiceKnob->get_value());
        cryptomatteContext.selectedIndex = lastIndex;
        // If the manifest sidecar file path isn't custom, check whether it has been changed
        if (cryptomatteContext.objects.empty() || getManifestSourceModified()) {
          cryptomatteContext.sidecarFilenameUiInvalid = false;
        }
        else {
          const auto& selectedCryptomatte = cryptomatteContext.objects[cryptomatteContext.selectedIndex];
          const auto& actualSidecarFilepath =
            GetAbsoluteFilePathToSidecarManifest(input(0), selectedCryptomatte.manifestFile);
          const auto sidecarFilepathFromUi = sidecarFilepathKnob->get_text();
          cryptomatteContext.sidecarFilenameUiInvalid = sidecarFilepathFromUi == nullptr
            ? !actualSidecarFilepath.empty()
            : actualSidecarFilepath != sidecarFilepathFromUi;
        }
      }
      else {
        // Otherwise, try find a Cryptomatte object among available that has the same name
        // as the name of last user-selected Cryptomatte object.
        const char* lastSelectedName = selectedCryptoLayerNameKnob->get_text();
        if (lastSelectedName != nullptr) {
          for (size_t i = 0; i < cryptomatteContext.objects.size(); ++i) {
            if (lastSelectedName == cryptomatteContext.objects[i].name) {
              cryptomatteContext.selectedIndex = i;
              break;
            }
          }
        }
      }
      return cryptomatteContext;
    }

    void CryptomattePlugin::saveLastSelectedCryptoLayerName(const CryptomatteContext& objectsAndState) const
    {
      // Don't reset the name of last user-selected Cryptomatte object, if there is no more available objects.
      if (objectsAndState.objects.empty()) {
        return;
      }
      auto lastSelectedCryptoLayerNameKnob = knob(kLastSelectedCryptoLayerNameKnob);
      if (lastSelectedCryptoLayerNameKnob != nullptr) {
        lastSelectedCryptoLayerNameKnob->set_text(objectsAndState.objects[objectsAndState.selectedIndex].name.c_str());
      }
    }

    void CryptomattePlugin::addObsoleteKnobs(Knob_Callback callback) const
    {
      // The following knob is supported by the plugin
      Obsolete_knob(callback, kRemoveChannelsKnob, "RemoveChannels", nullptr);

      // Helper lambda to create obsolete knobs with zero-padded names
      auto addPaddedObsoleteKnobs = [&callback](const std::string& baseKnobName,
                                                size_t numberOfKnobs) {
        if (numberOfKnobs != 0) {
          const size_t paddingWidth = std::to_string(numberOfKnobs - 1).size();
          for (size_t i = 0; i < numberOfKnobs; i++) {
            std::stringstream indexStream;
            indexStream << std::setw(paddingWidth) << std::setfill('0') << i;
            const std::string knobName = baseKnobName + indexStream.str();
            Obsolete_knob(callback, knobName.c_str(), nullptr);
          }
        }
      };

      // The following knobs are not needed to match the gizmo's functionality
      Obsolete_knob(callback, kLastSelectedCryptoLayerNameKnob, "cryptoLayer", nullptr);
      Obsolete_knob(callback, "cryptoLayerLock", nullptr);
      Obsolete_knob(callback, "expression", nullptr);
      Obsolete_knob(callback, "keyedName", nullptr);
      Obsolete_knob(callback, "manifestKey", nullptr);
      Obsolete_knob(callback, "previewChannel", nullptr);
      Obsolete_knob(callback, "stopAutoUpdate", nullptr);
      addPaddedObsoleteKnobs("in", 12);
      addPaddedObsoleteKnobs("previewExpression", 4);

      // Helper lambda to generate an obsolete knob with a warning attached
      auto addUnsupportedKnob = [&callback](const std::string& knobName) {
        const std::string warningMessage =
          "message \"Warning: detected use of unsupported knob '"
          + knobName + "' from the Cryptomatte gizmo.\"";
        Obsolete_knob(callback, knobName.c_str(), warningMessage.c_str());
      };

      // The following knobs are not currently supported
      addUnsupportedKnob("matteOnly");
      addUnsupportedKnob("previewMode");
      addUnsupportedKnob("singleSelection");
    }

    void CryptomattePlugin::updateCryptoLayerChoiceKnob(const CryptomatteContext& objectsAndState)
    {
      auto layerChoiceKnob = knob(kCryptoLayerChoiceKnob);
      if (layerChoiceKnob == nullptr) {
        return;
      }
      Enumeration_KnobI* layerChoiceEnumerationKnob = layerChoiceKnob->enumerationKnob();
      if (layerChoiceEnumerationKnob == nullptr) {
        return;
      }
      // Save the name of current user-selected Cryptomatte object.
      saveLastSelectedCryptoLayerName(objectsAndState);

      // If layerChoiceKnob has already been changed by an undo, but this undo is not valid e.g.
      // in the case the undo layer no longer exists in the CryptomatteContext, set_value() and
      // menu() will correct the layer selection but also create the same undo at the top of the
      // stack, causing an undo loop. To prevent this, guard against these functions creating undos.
      layerChoiceKnob->undoless(true);
      // Update selected index
      layerChoiceKnob->set_value(static_cast<double>(objectsAndState.selectedIndex));
      // Update menu items
      std::vector<std::string> names;
      names.reserve(objectsAndState.objects.size());
      for (const auto& cryptomatteObject : objectsAndState.objects) {
        names.push_back(cryptomatteObject.name);
      }
      layerChoiceEnumerationKnob->menu(names);
      // stop guarding against undos
      layerChoiceKnob->undoless(false);
    }

    void CryptomattePlugin::updateManifestSourceKnobs(const CryptomatteContext& cryptomatteContext)
    {
      if (cryptomatteContext.objects.empty()) {
        return;
      }
      const auto layerChoiceKnob = knob(kCryptoLayerChoiceKnob);
      auto manifestSourceKnob = knob(kManifestSourceKnob);
      auto sidecarFilenameKnob = knob(kSidecarFilepathKnob);
      if (layerChoiceKnob == nullptr || manifestSourceKnob == nullptr || sidecarFilenameKnob == nullptr) {
        return;
      }

      KnobProceduralModeficationSession session(this, kInsideManifestSourceProceduralModeficatorKnob);

      // Reset manifest source knobs to their default values, if they haven't been changed by the user yet.
      if (!getManifestSourceModified()) {
        const size_t selectedLayerIndex = static_cast<size_t>(layerChoiceKnob->get_value());
        if (selectedLayerIndex < cryptomatteContext.objects.size()) {
          const auto& selectedCryptomatte = cryptomatteContext.objects[selectedLayerIndex];
          if (selectedCryptomatte.manifestFile.empty()) {
            manifestSourceKnob->set_value(static_cast<double>(eEmbeddedManifest));
            sidecarFilenameKnob->set_text("");
          }
          else {
            manifestSourceKnob->set_value(static_cast<double>(eSidecarManifest));
            const std::string sidecarFilePath =
              GetAbsoluteFilePathToSidecarManifest(input(0), selectedCryptomatte.manifestFile);
            sidecarFilenameKnob->set_text(sidecarFilePath.c_str());
          }
        }
      }

      updateSidecarFilenameKnobEnabled();
    }

    void CryptomattePlugin::updateSidecarFilenameKnobEnabled()
    {
      const auto manifestSourceKnob = knob(kManifestSourceKnob);
      if (manifestSourceKnob == nullptr) {
        return;
      }
      auto sidecarFilenameKnob = knob(kSidecarFilepathKnob);
      if (sidecarFilenameKnob == nullptr) {
        return;
      }

      KnobProceduralModeficationSession session(this, kInsideManifestSourceProceduralModeficatorKnob);

      // Disable the sidecar filename knob by when the manifest is embedded; otherwise enable it.
      const bool isManifestSidecar = static_cast<size_t>(manifestSourceKnob->get_value()) == eSidecarManifest;
      sidecarFilenameKnob->enable(isManifestSidecar);
    }

    Manifest CryptomattePlugin::getManifestFromSelectedSource(const CryptomatteObject& cryptomatte)
    {
      const auto manifestSourceKnob = knob(kManifestSourceKnob);
      const auto sidecarFilenameKnob = knob(kSidecarFilepathKnob);
      if (manifestSourceKnob == nullptr || sidecarFilenameKnob == nullptr) {
        return Manifest();
      }

      const auto selectedManifestSource = static_cast<size_t>(manifestSourceKnob->get_value());
      const auto manifestSource = static_cast<ManifestSource>(selectedManifestSource);
      const char* arrSidecarFilename = sidecarFilenameKnob->get_text(&uiContext());

      Manifest manifest;
      if (manifestSource == eEmbeddedManifest) {
        manifest = GetManifestEmbedded(input(0), cryptomatte);
      }
      else if (arrSidecarFilename != nullptr) {
        const std::string sidecarFilename(arrSidecarFilename);
        manifest = GetManifestSidecar(arrSidecarFilename);

        if (manifest.empty()) {
          const std::string errorMessage = R"(")" + sidecarFilename
                                         + R"(" is not a valid sidecar manifest file path)";
          error(errorMessage.c_str());
        }
      }

      return manifest;
    }

    bool CryptomattePlugin::getManifestSourceModified() const
    {
      const auto manifestSourceModifiedKnob = knob(kManifestSourceModifiedKnob);
      if (manifestSourceModifiedKnob != nullptr) {
        return static_cast<bool>(manifestSourceModifiedKnob->get_value());
      }

      return false;
    }

    void CryptomattePlugin::checkManifestSourceModified(CryptomattePlugin::CryptomatteContext context)
    {
      if (context.objects.empty()) {
        return;
      }
      const auto sidecarFilepathKnob = knob(kSidecarFilepathKnob);
      const auto manifestSourceKnob = knob(kManifestSourceKnob);
      auto manifestSourceModifiedKnob = knob(kManifestSourceModifiedKnob);
      if (sidecarFilepathKnob == nullptr || manifestSourceKnob == nullptr || manifestSourceModifiedKnob == nullptr) {
        return;
      }
      bool manifestSourceModified = manifestSourceModifiedKnob->get_value() != 0.0;
      if (manifestSourceModified) {
        // Nothing more to do here, when the flag is already set.
        return;
      }
      if (KnobProceduralModeficationSession::AtProceduralModefication(this, kInsideManifestSourceProceduralModeficatorKnob)) {
        // Skip procedural modifications of the manifest source knobs.
        return;
      }

      const auto& selectedObject = context.objects[context.selectedIndex];
      const auto manifestSource = static_cast<ManifestSource>(static_cast<size_t>((manifestSourceKnob->get_value())));
      if (selectedObject.manifestFile.empty() && manifestSource != eSidecarManifest) {
        manifestSourceModifiedKnob->set_value(1.0);
        return;
      }

      const auto& uiManifestFilename = sidecarFilepathKnob->get_text();
      const auto& metadataManifestFilename =
        GetAbsoluteFilePathToSidecarManifest(input(0), selectedObject.manifestFile);
      manifestSourceModified = uiManifestFilename == nullptr
        ? !metadataManifestFilename.empty()
        : uiManifestFilename != metadataManifestFilename;
      manifestSourceModifiedKnob->set_value(static_cast<double>(manifestSourceModified));
    }

    static Iop* build(Node* node) { return new CryptomattePlugin(node); }
    const Iop::Description CryptomattePlugin::kDescription("Cryptomatte", nullptr, build);

  }
}
