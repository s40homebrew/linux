/* SPDX-License-Identifier: MIT */
import javax.microedition.lcdui.Alert;
import javax.microedition.lcdui.AlertType;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.CommandListener;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Displayable;

public final class Linux6303MmuProbe extends HomebrewMIDlet {
    protected void startApp() {
        String platform=null;
        try { platform=System.getProperty(FirmwareProfiles.PLATFORM_PROPERTY); }
        catch(SecurityException unavailable) {}
        FirmwareProfile profile=FirmwareProfiles.select(platform);
        if(profile==null || !FirmwareProfiles.RM443_BOOTSTRAP.equals(profile.bootstrapId)) {
            Alert alert=new Alert("6303 classic only",
                    "This MMU experiment requires Nokia 6303 classic (RM-443).",null,AlertType.ERROR);
            alert.setTimeout(Alert.FOREVER);
            alert.addCommand(new Command("Exit",Command.EXIT,1));
            alert.setCommandListener(new CommandListener() {
                public void commandAction(Command c,Displayable d){notifyDestroyed();}
            });
            Display.getDisplay(this).setCurrent(alert);return;
        }
        super.startApp();
    }
    protected String applicationName(){return "Linux 6303 MMU experiment";}
    protected String startLabel(){return "Capture MMU tables";}
    protected String[] instructions(int inputLayout) {
        return new String[]{"6303 classic / RM-443 only","Emulator-first MMU experiment",
            "Briefly gates domain access","Masks IRQ/FIQ, copies tables",
            "Restores CPU state afterward","May crash on unexpected mapping",
            "Does not start Linux","Choose Check firmware first"};
    }
}
