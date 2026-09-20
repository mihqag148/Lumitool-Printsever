using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        try
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string script = Path.Combine(baseDir, "Lumitool_Printsever_Setup_V4_1.ps1");

            if (!File.Exists(script))
            {
                MessageBox.Show(
                    "Không tìm thấy file giao diện Lumitool Printsever.",
                    "Lumitool Printsever",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error
                );
                return;
            }

            var psi = new ProcessStartInfo
            {
                FileName = "powershell.exe",
                Arguments = "-NoLogo -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + "\"",
                WorkingDirectory = baseDir,
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden
            };

            using (var p = Process.Start(psi))
            {
                if (p == null)
                {
                    MessageBox.Show(
                        "Không khởi động được Lumitool Printsever.",
                        "Lumitool Printsever",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Error
                    );
                    return;
                }

                p.WaitForExit();

                if (p.ExitCode != 0)
                {
                    MessageBox.Show(
                        "Lumitool Printsever gặp lỗi.\r\n\r\nLog: %TEMP%\\Lumitool_Printsever_Setup_V4_1.log",
                        "Lumitool Printsever",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Error
                    );
                }
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                ex.Message,
                "Lumitool Printsever",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error
            );
        }
    }
}
