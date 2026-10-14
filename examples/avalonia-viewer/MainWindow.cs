using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using LibBdf;

namespace AvaloniaViewer;

/// <summary>
/// The window: a toolbar (open, the view, the pages, zoom) over the page,
/// which scrolls when it is larger than the window.
/// </summary>
public sealed class MainWindow : Window
{
    private readonly PageView _pageView = new();
    private readonly ScrollViewer _scroll;
    private readonly ComboBox _views = new() { MinWidth = 140 };
    private readonly TextBlock _label = new() { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0) };
    private Viewer? _viewer;
    private int _view, _page;
    private double _zoom; // 0: fit the window

    public MainWindow(string? path)
    {
        Title = "BDF";
        Width = 960;
        Height = 720;
        Button Tool(string text, Action action, KeyGesture? key = null)
        {
            var b = new Button { Content = text, Margin = new Thickness(2) };
            b.Click += (_, _) => action();
            if (key != null)
            {
                b.HotKey = key;
            }
            return b;
        }
        var bar = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(4) };
        bar.Children.Add(Tool("Open…", () => _ = Choose(), new KeyGesture(Key.O, KeyModifiers.Control)));
        bar.Children.Add(_views);
        bar.Children.Add(Tool("‹", () => Turn(-1), new KeyGesture(Key.Left)));
        bar.Children.Add(_label);
        bar.Children.Add(Tool("›", () => Turn(1), new KeyGesture(Key.Right)));
        bar.Children.Add(Tool("−", () => ZoomBy(0.8), new KeyGesture(Key.OemMinus, KeyModifiers.Control)));
        bar.Children.Add(Tool("Fit", () => { _zoom = 0; Update(); }, new KeyGesture(Key.D0, KeyModifiers.Control)));
        bar.Children.Add(Tool("+", () => ZoomBy(1.25), new KeyGesture(Key.OemPlus, KeyModifiers.Control)));
        _views.SelectionChanged += (_, _) =>
        {
            if (_viewer != null && _views.SelectedIndex >= 0)
            {
                _view = _viewer.Views[_views.SelectedIndex].Index;
                _page = 0;
                Update();
            }
        };
        _scroll = new ScrollViewer
        {
            Content = new Border { Child = _pageView, Margin = new Thickness(12), HorizontalAlignment = HorizontalAlignment.Center },
            HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto,
            Background = new SolidColorBrush(Color.FromRgb(0x55, 0x58, 0x5e)),
        };
        var root = new DockPanel();
        DockPanel.SetDock(bar, Dock.Top);
        root.Children.Add(bar);
        root.Children.Add(_scroll);
        Content = root;
        _scroll.SizeChanged += (_, _) => { if (_zoom == 0) Update(); };
        Closed += (_, _) => Close(_viewer);
        if (path != null)
        {
            Opened += async (_, _) => await Open(path, File.ReadAllBytes(path));
        }
    }

    private async Task Choose()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Open a BDF document",
            FileTypeFilter = new[] { new FilePickerFileType("BDF") { Patterns = new[] { "*.bdf" } } },
        });
        if (files.Count == 0)
        {
            return;
        }
        await using var stream = await files[0].OpenReadAsync();
        using var bytes = new MemoryStream();
        await stream.CopyToAsync(bytes);
        await Open(files[0].Name, bytes.ToArray());
    }

    private async Task Open(string name, byte[] data)
    {
        BdfDocument document;
        try
        {
            document = new BdfDocument(data);
        }
        catch (BdfException e)
        {
            await Message($"{Path.GetFileName(name)}: {e.Message}");
            return;
        }
        // an encrypted document: its views come with the password
        for (string note = "The document is encrypted."; document.IsLocked;)
        {
            string? password = await new PasswordDialog(note).ShowDialog<string?>(this);
            if (password == null)
            {
                document.Dispose();
                return;
            }
            try
            {
                document.Unlock(password);
            }
            catch (BdfException)
            {
                note = "The password does not open it. Try again.";
            }
        }
        var old = _viewer;
        _viewer = new Viewer(document);
        Title = Path.GetFileName(name);
        _views.ItemsSource = _viewer.Views.Select(v => string.IsNullOrEmpty(v.View.Title) ? v.View.Id : v.View.Title).ToList();
        _view = _viewer.Views.FirstOrDefault().Index;
        _page = 0;
        _zoom = 0;
        _views.SelectedIndex = _viewer.Views.Count > 0 ? 0 : -1;
        Update();
        Close(old);
    }

    private void Close(Viewer? viewer)
    {
        if (viewer != null && viewer != _viewer)
        {
            viewer.Dispose();
        }
        else if (viewer != null && !IsVisible)
        {
            _pageView.Show(null, default, 0, 0, 1);
            viewer.Dispose();
        }
    }

    private void Turn(int by)
    {
        int count = _viewer?.Document.Views[_view].PageCount ?? 0;
        if (_page + by >= 0 && _page + by < count)
        {
            _page += by;
            Update();
        }
    }

    private void ZoomBy(double factor)
    {
        _zoom = Math.Clamp((_zoom > 0 ? _zoom : FitZoom()) * factor, 0.05, 32);
        Update();
    }

    private double FitZoom()
    {
        var size = _viewer?.Document.PageSize(_page, _view);
        if (size is not { } s || s.Width <= 0 || s.Height <= 0)
        {
            return 1;
        }
        var room = _scroll.Bounds.Size;
        return Math.Max(0.05, Math.Min((room.Width - 32) / s.Width, (room.Height - 32) / s.Height));
    }

    private void Update()
    {
        if (_viewer == null || _viewer.Views.Count == 0)
        {
            _label.Text = "";
            return;
        }
        var view = _viewer.Document.Views[_view];
        var size = _viewer.Document.PageSize(_page, _view) ?? (0, 0);
        _label.Text = $"{_page + 1} / {view.PageCount}";
        _pageView.Show(_viewer.Skia, new Size(size.Width, size.Height), _view, _page, _zoom > 0 ? _zoom : FitZoom());
    }

    private async Task Message(string text)
    {
        var ok = new Button { Content = "OK", HorizontalAlignment = HorizontalAlignment.Right };
        var dialog = new Window
        {
            Title = "BDF", Width = 420, SizeToContent = SizeToContent.Height,
            Content = new StackPanel { Margin = new Thickness(16), Spacing = 12, Children = { new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap }, ok } },
        };
        ok.Click += (_, _) => dialog.Close();
        await dialog.ShowDialog(this);
    }
}

/// <summary>Asks for the password of an encrypted document.</summary>
public sealed class PasswordDialog : Window
{
    public PasswordDialog(string note)
    {
        Title = "Password";
        Width = 360;
        SizeToContent = SizeToContent.Height;
        var box = new TextBox { PasswordChar = '●', PlaceholderText = "Password" };
        var open = new Button { Content = "Open", IsDefault = true };
        var cancel = new Button { Content = "Cancel", IsCancel = true };
        open.Click += (_, _) => Close(box.Text ?? "");
        cancel.Click += (_, _) => Close(null);
        Content = new StackPanel
        {
            Margin = new Thickness(16), Spacing = 12,
            Children =
            {
                new TextBlock { Text = note },
                box,
                new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Spacing = 8, Children = { cancel, open } },
            },
        };
        Opened += (_, _) => box.Focus();
    }
}
