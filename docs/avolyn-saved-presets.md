# Three saved presets (0.2.7-dev)

The XIAO ESP32-S3 can keep **three named 792 × 272 B/W layout snapshots** in flash. Saving a slot selects it. The selected valid layout is loaded into RAM at boot, including after wall-power restart without a computer. USB remains the transfer/control transport; physical screen refresh and hardware-button cycling are separate work.

## Using the feature

With matching desktop/firmware changes installed after approval, open **Your Slate → Upload layout over USB**. Choose slot 1, 2 or 3 and save the current editor layout. Repeat with different layouts in the other slots. The panel lists names, empty/corrupt slots and the active slot. **Activate** loads a saved layout and remembers that selection without uploading pixels again.

Snapshots contain the data rendered at save time. Calendar/PC widgets do not become live firmware applications. New data requires a new layout from the desktop. Presets survive normal restart/power-off; erasing all flash or formatting the filesystem removes them. Physical display support still reports false.

## Storage and failure handling

The existing XIAO `default_8MB.csv` has a `spiffs` data partition at 0x670000, size 0x180000 (1.5 MiB). This PR does not change partitions or touch Wi-Fi/pairing NVS. LittleFS uses that data partition and an owned `avolyn` directory. Mounting never requests format-on-error. An initial format is allowed only after reading and verifying that the entire partition is erased (0xFF). Existing non-LittleFS or corrupt filesystem contents are preserved and persistent storage reports unavailable.

Three images use 80,784 bytes. A fixed catalog contains a 1,024-byte integrity-checked header plus all three image regions: **81,808 bytes**. Named slot records carry a stable source-preset identifier, stored frame identifier, name and pixel SHA-256. The metadata has its own SHA-256. Only the active image, incoming staging image and one verification buffer are needed in RAM; all three are not cached simultaneously.

A changed save writes a complete temporary catalog, checks write/flush/fsync/close results, reads back its header and image checksums, then uses LittleFS atomic rename to replace the live catalog. Images and selected slot therefore commit together. Failed writes, failed verification or failed rename leave the old catalog in place. A dangling temporary file from interruption is ignored at boot. Repeated COMMIT requests are idempotent. Identical content/name/source identity in the already-selected slot does not rewrite flash.

Manual slot activation also commits the selected slot through the catalog. This favors one recoverable transaction; it rewrites approximately 82 KB for a changed selection. It is not a high-frequency button-cycling or automated update policy. Background status/boot checks never save the catalog. The adapter yields during writes; persistent COMMIT and selection have a longer bounded desktop acknowledgement timeout.

Boot verifies metadata and each occupied image. An image checksum failure marks that slot invalid without deleting it; other valid slots remain available. If the selected image is invalid, boot uses the first valid slot without writing flash. A corrupt catalog header disables persistent changes and preserves the file. No automatic cleanup erases unrelated files.

LittleFS's documented power-loss behavior: [littlefs README](https://github.com/littlefs-project/littlefs/blob/v2.9.3/README.md). The adapter uses the ESP32 VFS sync/rename operations. Automated tests simulate interrupted operations; real power-cut and filesystem initialization tests on the XIAO remain required.

## USB protocol

The original `avolyn-frame-v1` RAM upload remains supported. `FRAME_CAPS.persistent` stays false for that default contract. Additional capabilities advertise `presetProtocol: avolyn-presets-v1`, `maxPresets: 3` and `presetStorageReady`.

To save durably, FRAME_BEGIN adds `presetProtocol`, integer `slot` (1–3), lowercase 32-hex `presetId` and a name of 1–64 UTF-8 bytes. Names reject control characters and embedded NUL. Duplicate BEGIN must also match this metadata. All image bounds, chunk offsets and SHA-256 rules remain in force. An unavailable filesystem rejects a persistent BEGIN; no silent RAM fallback occurs.

Persistent COMMIT acknowledges `persistent: true`, `activePresetSlot`, pixel SHA-256 and `storedFrameId`. Only a successful durable save allows the RAM receiver to accept the staged frame. `acceptedFrameId` correlates the current transfer; deduplication can retain an older `storedFrameId` for unchanged content. On reboot, the stored frame identifier is restored.

`PRESET_LIST` returns exactly three bounded slot records and the active/selected slots. `PRESET_SELECT` requires slot and expected pixel SHA-256 to reject stale selection. Selection is rejected while another image is staged. Commands and responses share the existing request-ID correlation and connection-bound USB queue. No new LAN route or pairing-key change is added.

## Validation

`python scripts/test_preset_store.py` compiles the actual portable store, POSIX adapter, receiver and ArduinoJson bridge with address/undefined-behavior sanitizers. It covers three distinct images, restart restoration, slot switching, invalid/stale selection, unchanged-save deduplication, short write/sync/verification/rename/read faults, leftover temporary files, corruption and repair, and non-destructive mount policy. The existing frame/USB burst, Wi-Fi and cryptographic tests remain enabled. CI compiles the real XIAO target without uploading it.

Hardware acceptance after approval: save three distinct presets, select each, power-cycle, confirm all names/digests and the selected slot, then interrupt a replacement while maintaining independent power. Check that Wi-Fi recovery remains responsive and that existing presets survive a failed transfer. No agent has merged or flashed these changes.
