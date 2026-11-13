/* SPDX-License-Identifier: MIT */
import javax.microedition.lcdui.Alert;
import javax.microedition.lcdui.AlertType;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Displayable;
import javax.microedition.rms.RecordStore;
import javax.microedition.rms.RecordStoreException;

public final class Nokia6303Linux extends HomebrewMIDlet {
    private static final int DMA = 1, RGB666 = 2, RGB888 = 4, COLOR_MASK = 6;
    private final Command usePio = new Command("Use PIO", Command.SCREEN, 3);
    private final Command useDma = new Command("Use DMA", Command.SCREEN, 3);
    private final Command use16bit = new Command("Use 16-bit color", Command.SCREEN, 4);
    private final Command use18bit = new Command("Use 18-bit color", Command.SCREEN, 4);
    private final Command use24bit = new Command("Use 24-bit color", Command.SCREEN, 4);
    private int options = DMA | RGB888;
    private boolean settingsLoaded, settingsEnabled;

    protected void startApp() {
        String platform = null;
        try { platform = System.getProperty(FirmwareProfiles.PLATFORM_PROPERTY); }
        catch (SecurityException unavailable) {}
        FirmwareProfile profile = FirmwareProfiles.select(platform);
        if (profile == null || !FirmwareProfiles.RM443_BOOTSTRAP.equals(profile.bootstrapId)) {
            Alert alert = new Alert("Unsupported device", "Requires Nokia 6303 classic (RM-443).", null, AlertType.ERROR);
            alert.setTimeout(Alert.FOREVER); Display.getDisplay(this).setCurrent(alert); return;
        }
        if (!settingsLoaded) {
            settingsLoaded = true;
            loadSettings();
        }
        super.startApp();
    }
    protected String applicationName() { return "Linux"; }
    protected String nativeResource() { return "/application.bin.parts"; }
    protected String startLabel() { return "Start Linux"; }
    protected boolean launchDiagnostics() { return true; }
    protected int nativeOptions() { return options; }
    protected void configureCommands(Displayable target, boolean enabled) {
        target.removeCommand(usePio);
        target.removeCommand(useDma);
        target.removeCommand(use16bit);
        target.removeCommand(use18bit);
        target.removeCommand(use24bit);
        settingsEnabled = enabled;
        if (enabled) {
            target.addCommand((options & DMA) != 0 ? usePio : useDma);
            int color = options & COLOR_MASK;
            target.addCommand(color == RGB888 ? use18bit : color == RGB666 ? use16bit : use24bit);
        }
    }

    public void commandAction(Command command, Displayable target) {
        if (command == usePio || command == useDma ||
                command == use16bit || command == use18bit || command == use24bit) {
            if (!settingsEnabled) return;
            if (command == usePio) options &= ~DMA;
            else if (command == useDma) options |= DMA;
            else {
                options &= ~COLOR_MASK;
                if (command == use18bit) options |= RGB666;
                else if (command == use24bit) options |= RGB888;
            }
            saveSettings();
            configureCommands(target, true);
            refreshInstructions();
        } else {
            super.commandAction(command, target);
        }
    }

    protected String[] instructions(int layout) {
        return new String[] {"Version " + getAppProperty("MIDlet-Version"),
                "Graphics: " + ((options & DMA) != 0 ? "DMA" : "PIO"),
                "Color: " + ((options & COLOR_MASK) == RGB888 ? "24-bit" :
                        (options & COLOR_MASK) == RGB666 ? "18-bit" : "16-bit")};
    }

    private void loadSettings() {
        RecordStore store = null;
        try {
            store = RecordStore.openRecordStore("linux-display", true);
            if (store.getNumRecords() != 0) {
                byte[] value = store.getRecord(1);
                if (value.length == 1 && (value[0] & ~7) == 0 &&
                        (value[0] & COLOR_MASK) != COLOR_MASK)
                    options = value[0];
            }
        } catch (RecordStoreException unavailable) {
        } finally {
            closeSettings(store);
        }
    }

    private void saveSettings() {
        RecordStore store = null;
        try {
            store = RecordStore.openRecordStore("linux-display", true);
            byte[] value = {(byte) options};
            if (store.getNumRecords() == 0) store.addRecord(value, 0, 1);
            else store.setRecord(1, value, 0, 1);
        } catch (RecordStoreException unavailable) {
        } finally {
            closeSettings(store);
        }
    }

    private static void closeSettings(RecordStore store) {
        if (store != null) {
            try { store.closeRecordStore(); }
            catch (RecordStoreException unavailable) {}
        }
    }
}
