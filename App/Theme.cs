using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace GamePadRemapper
{
    internal static class Theme
    {
        internal static readonly Brush Background = Brush("#0A1020");
        internal static readonly Brush Panel = Brush("#111A2E");
        internal static readonly Brush Raised = Brush("#18243B");
        internal static readonly Brush Muted = Brush("#91A1BD");
        internal static readonly Brush Text = Brush("#EDF2FE");
        internal static readonly Brush Accent = Brush("#A79AFF");
        internal static readonly Brush Mint = Brush("#7EE7CE");
        internal static readonly Brush Danger = Brush("#FF9EAA");
        internal static bool ReducedMotion;

        internal static SolidColorBrush Brush(string color)
        {
            var brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(color));
            brush.Freeze();
            return brush;
        }

        internal static void Apply(Application application)
        {
            const string resources = @"
<ResourceDictionary xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>
 <SolidColorBrush x:Key='Muted' Color='#91A1BD'/>
 <Style TargetType='TextBlock'><Setter Property='Foreground' Value='#EDF2FE'/><Setter Property='FontFamily' Value='Segoe UI'/></Style>
 <Style TargetType='Button'>
  <Setter Property='Foreground' Value='#EDF2FE'/><Setter Property='Background' Value='#21304C'/><Setter Property='BorderBrush' Value='#334461'/><Setter Property='BorderThickness' Value='1'/><Setter Property='Padding' Value='15,10'/><Setter Property='FontWeight' Value='SemiBold'/><Setter Property='Cursor' Value='Hand'/><Setter Property='HorizontalContentAlignment' Value='Center'/>
  <Setter Property='Template'><Setter.Value><ControlTemplate TargetType='Button'><Border x:Name='chrome' Background='{TemplateBinding Background}' BorderBrush='{TemplateBinding BorderBrush}' BorderThickness='{TemplateBinding BorderThickness}' CornerRadius='9'><ContentPresenter Margin='{TemplateBinding Padding}' HorizontalAlignment='{TemplateBinding HorizontalContentAlignment}' VerticalAlignment='Center'/></Border><ControlTemplate.Triggers><Trigger Property='IsMouseOver' Value='True'><Setter TargetName='chrome' Property='BorderBrush' Value='#A79AFF'/><Setter TargetName='chrome' Property='Opacity' Value='0.86'/></Trigger><Trigger Property='IsPressed' Value='True'><Setter TargetName='chrome' Property='Opacity' Value='0.65'/></Trigger><Trigger Property='IsEnabled' Value='False'><Setter TargetName='chrome' Property='Opacity' Value='0.4'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter>
 </Style>
 <Style x:Key='PrimaryButton' TargetType='Button' BasedOn='{StaticResource {x:Type Button}}'><Setter Property='Background' Value='#8B76F9'/><Setter Property='BorderBrush' Value='#A99AFE'/><Setter Property='Foreground' Value='#FFFFFF'/></Style>
 <Style x:Key='QuietButton' TargetType='Button' BasedOn='{StaticResource {x:Type Button}}'><Setter Property='Background' Value='Transparent'/><Setter Property='BorderBrush' Value='#2B3A55'/><Setter Property='Padding' Value='11,7'/></Style>
 <Style TargetType='TextBox'><Setter Property='Foreground' Value='#EDF2FE'/><Setter Property='Background' Value='#0C1527'/><Setter Property='BorderBrush' Value='#334461'/><Setter Property='BorderThickness' Value='1'/><Setter Property='Padding' Value='10,8'/><Setter Property='CaretBrush' Value='#C2B8FF'/><Setter Property='SelectionBrush' Value='#6352A5'/></Style>
 <Style TargetType='ComboBox'><Setter Property='Foreground' Value='#17243C'/><Setter Property='Background' Value='#DFE6F4'/><Setter Property='Padding' Value='8,5'/><Setter Property='MinHeight' Value='30'/><Setter Property='VerticalContentAlignment' Value='Center'/><Setter Property='ItemTemplate'><Setter.Value><DataTemplate><TextBlock Text='{Binding}' Foreground='#17243C'/></DataTemplate></Setter.Value></Setter></Style>
 <Style TargetType='CheckBox'><Setter Property='Foreground' Value='#C9D5E9'/><Setter Property='Margin' Value='0,4'/><Setter Property='VerticalContentAlignment' Value='Center'/></Style>
 <Style TargetType='ToolTip'><Setter Property='Background' Value='#21304C'/><Setter Property='Foreground' Value='#EDF2FE'/><Setter Property='Padding' Value='10,7'/></Style>
 <Style TargetType='Thumb'><Setter Property='Background' Value='#40516F'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='Thumb'><Border Background='{TemplateBinding Background}' CornerRadius='5' Margin='2'/></ControlTemplate></Setter.Value></Setter><Style.Triggers><Trigger Property='IsMouseOver' Value='True'><Setter Property='Background' Value='#8F83CC'/></Trigger></Style.Triggers></Style>
 <Style TargetType='ScrollBar'><Setter Property='Background' Value='#111B2C'/><Setter Property='Width' Value='12'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ScrollBar'><Grid Background='{TemplateBinding Background}'><Track x:Name='PART_Track' IsDirectionReversed='True' Orientation='{TemplateBinding Orientation}' Minimum='{TemplateBinding Minimum}' Maximum='{TemplateBinding Maximum}' Value='{TemplateBinding Value}' ViewportSize='{TemplateBinding ViewportSize}'><Track.DecreaseRepeatButton><RepeatButton x:Name='decrease' Command='{x:Static ScrollBar.PageUpCommand}' Opacity='0' IsTabStop='False'/></Track.DecreaseRepeatButton><Track.Thumb><Thumb MinHeight='20' MinWidth='8'/></Track.Thumb><Track.IncreaseRepeatButton><RepeatButton x:Name='increase' Command='{x:Static ScrollBar.PageDownCommand}' Opacity='0' IsTabStop='False'/></Track.IncreaseRepeatButton></Track></Grid><ControlTemplate.Triggers><Trigger Property='Orientation' Value='Horizontal'><Setter TargetName='PART_Track' Property='IsDirectionReversed' Value='False'/><Setter TargetName='decrease' Property='Command' Value='{x:Static ScrollBar.PageLeftCommand}'/><Setter TargetName='increase' Property='Command' Value='{x:Static ScrollBar.PageRightCommand}'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter><Style.Triggers><Trigger Property='Orientation' Value='Horizontal'><Setter Property='Width' Value='Auto'/><Setter Property='Height' Value='12'/></Trigger></Style.Triggers></Style>
 <Style TargetType='ListBox'><Setter Property='Background' Value='Transparent'/><Setter Property='BorderThickness' Value='0'/><Setter Property='Foreground' Value='#EDF2FE'/><Setter Property='Padding' Value='0'/><Setter Property='HorizontalContentAlignment' Value='Stretch'/></Style>
 <Style TargetType='ListBoxItem'><Setter Property='Padding' Value='0'/><Setter Property='Margin' Value='0,3'/><Setter Property='HorizontalContentAlignment' Value='Stretch'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ListBoxItem'><Border x:Name='row' CornerRadius='9' Padding='12,10' Background='Transparent'><ContentPresenter/></Border><ControlTemplate.Triggers><Trigger Property='IsSelected' Value='True'><Setter TargetName='row' Property='Background' Value='#2A284B'/></Trigger><Trigger Property='IsMouseOver' Value='True'><Setter TargetName='row' Property='BorderBrush' Value='#71639F'/><Setter TargetName='row' Property='BorderThickness' Value='1'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>
</ResourceDictionary>";
            application.Resources.MergedDictionaries.Add((ResourceDictionary)XamlReader.Parse(resources));
            ReducedMotion = !SystemParameters.ClientAreaAnimation;
        }

        internal static void Reveal(UIElement element)
        {
            if (ReducedMotion) return;
            element.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0.5, 1, TimeSpan.FromMilliseconds(150)) { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut } });
        }

        internal static TextBlock Label(string value, double size = 13, Brush foreground = null)
        {
            return new TextBlock { Text = value, FontSize = size, Foreground = foreground ?? Text, TextWrapping = TextWrapping.Wrap };
        }

        internal static Border Card(UIElement child, Thickness? padding = null)
        {
            return new Border { Child = child, CornerRadius = new CornerRadius(15), Background = Panel, BorderBrush = Brush("#24314B"), BorderThickness = new Thickness(1), Padding = padding ?? new Thickness(20) };
        }
    }
}
