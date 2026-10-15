// "MW3DS Converter.exe" in the release converter: starts the bundled Python on the wizard (app\tools\release\wizard.py).
// Built by tools/release/make_release.py with the C# compiler that comes with Windows (.NET Framework 4), so it runs
// with nothing installed.
using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;

static class Launcher
{
	[STAThread]
	static int Main()
	{
		string dir = AppDomain.CurrentDomain.BaseDirectory;
		string python = Path.Combine(dir, "python", "pythonw.exe");
		string wizard = Path.Combine(dir, "app", "tools", "release", "wizard.py");
		if (!File.Exists(python) || !File.Exists(wizard))
		{
			MessageBox.Show("The converter's files are missing next to this program.\n\n" +
				"Unzip the whole download into a folder first (right-click the zip, Extract All), " +
				"then run MW3DS Converter.exe from that folder.", "MW3DS Converter",
				MessageBoxButtons.OK, MessageBoxIcon.Error);
			return 1;
		}
		ProcessStartInfo start = new ProcessStartInfo(python, "\"" + wizard + "\"");
		start.UseShellExecute = false;
		start.WorkingDirectory = dir;
		Process.Start(start);
		return 0;
	}
}
