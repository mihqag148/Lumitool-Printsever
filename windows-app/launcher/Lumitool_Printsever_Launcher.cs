using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Windows.Forms;

internal static class Program
{
    private static bool IsAdministrator()
    {
        using (WindowsIdentity identity = WindowsIdentity.GetCurrent())
        {
            WindowsPrincipal principal = new WindowsPrincipal(identity);
            return principal.IsInRole(WindowsBuiltInRole.Administrator);
        }
    }

    private static bool RelaunchElevated(string baseDir)
    {
        try
        {
            var elevate = new ProcessStartInfo
            {
                FileName = Application.ExecutablePath,
                WorkingDirectory = baseDir,
                UseShellExecute = true,
                Verb = "runas"
            };

            Process.Start(elevate);
            return true;
        }
        catch (Win32Exception ex)
        {
            if (ex.NativeErrorCode == 1223)
            {
                MessageBox.Show(
                    "Lumitool Printsever cần quyền Administrator để cài và quản lý máy in.\r\n\r\nBạn đã hủy yêu cầu cấp quyền.",
                    "Lumitool Printsever",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Warning
                );
                return false;
            }

            MessageBox.Show(
                "Không thể yêu cầu quyền Administrator.\r\n\r\n" + ex.Message,
                "Lumitool Printsever",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error
            );
            return false;
        }
    }

    [STAThread]
    private static void Main()
    {
        try
        {
            string baseDir = AppDomain.CurrentDomain.BaseDirectory;

            // IMPORTANT:
            // The EXE manifest is asInvoker so Inno Setup can launch it without
            // CreateProcess error 740. If the current process is not elevated,
            // request elevation explicitly through ShellExecute "runas".
            if (!IsAdministrator())
            {
                RelaunchElevated(baseDir);
                return;
            }

            string script = Path.Combine(baseDir, "Lumitool_Printsever_Setup_V4_7.ps1");

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
                        "Lumitool Printsever gặp lỗi.\r\n\r\nLog: %TEMP%\\Lumitool_Printsever_Setup_V4_7.log",
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
