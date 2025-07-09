# VortiDeck Banner CSS Animation Guide

## Overview

The VortiDeck banner system creates **full-screen transparent browser sources** positioned at (0,0) that overlay your entire OBS canvas. When you provide custom CSS, the system automatically creates an HTML wrapper structure that your CSS can target for animations.

## HTML Structure Created

When you send an image/video URL with custom CSS, the plugin creates this structure:

```html
<html>
<head>
<style>
  /* Your custom CSS goes here */
</style>
</head>
<body>
  <img src="https://your-image-url.gif">
  <!-- OR for videos: -->
  <video src="https://your-video-url.mp4" autoplay loop muted></video>
</body>
</html>
```

## Browser Source Properties

- **Size**: Full OBS canvas (1920x1080, etc.)
- **Position**: Always at (0,0) - top-left corner
- **Background**: Transparent
- **Overlay**: Everything underneath remains visible

## Basic CSS Structure

Your CSS should follow this pattern:

```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  position: relative;
}

img {
  /* Your image styling and animations */
}

video {
  /* Your video styling and animations */
}
```

## Animation Examples

### 1. Corner Bouncing Animation

**Effect**: Image jumps between screen corners every 2 seconds

```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  position: relative;
}

img {
  width: 200px;
  height: 200px;
  object-fit: contain;
  border: none;
  position: absolute;
  top: 0;
  left: 0;
  animation: cornerBounce 8s step-start infinite;
}

@keyframes cornerBounce {
  0% { transform: translate(0, 0); } /* Top-left */
  25% { transform: translate(calc(100vw - 200px), 0); } /* Top-right */
  50% { transform: translate(calc(100vw - 200px), calc(100vh - 200px)); } /* Bottom-right */
  75% { transform: translate(0, calc(100vh - 200px)); } /* Bottom-left */
  100% { transform: translate(0, 0); } /* Back to top-left */
}
```

### 2. Smooth Floating Animation

**Effect**: Image smoothly floats around the screen

```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  position: relative;
}

img {
  width: 150px;
  height: 150px;
  object-fit: contain;
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  animation: smoothFloat 10s ease-in-out infinite;
}

@keyframes smoothFloat {
  0% { transform: translate(-50%, -50%) rotate(0deg); }
  25% { transform: translate(300px, -200px) rotate(90deg); }
  50% { transform: translate(-300px, -200px) rotate(180deg); }
  75% { transform: translate(-300px, 200px) rotate(270deg); }
  100% { transform: translate(-50%, -50%) rotate(360deg); }
}
```

### 3. Pulsing Scale Animation

**Effect**: Image pulses in size while staying centered

```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  display: flex;
  align-items: center;
  justify-content: center;
}

img {
  width: 200px;
  height: 200px;
  object-fit: contain;
  animation: pulse 3s ease-in-out infinite;
}

@keyframes pulse {
  0% { transform: scale(1); }
  50% { transform: scale(1.5); }
  100% { transform: scale(1); }
}
```

### 4. Slide In/Out Animation

**Effect**: Image slides in from left, stays, then slides out to right

```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  position: relative;
}

img {
  width: 300px;
  height: 100px;
  object-fit: contain;
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  animation: slideInOut 6s ease-in-out infinite;
}

@keyframes slideInOut {
  0% { left: -300px; } /* Start off-screen left */
  20% { left: 50%; transform: translate(-50%, -50%); } /* Center */
  80% { left: 50%; transform: translate(-50%, -50%); } /* Stay centered */
  100% { left: 100vw; } /* Exit off-screen right */
}
```

### 5. Random Position Animation

**Effect**: Image appears at random positions

```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  position: relative;
}

img {
  width: 150px;
  height: 150px;
  object-fit: contain;
  position: absolute;
  animation: randomJump 12s step-start infinite;
}

@keyframes randomJump {
  0% { top: 10%; left: 20%; }
  16.66% { top: 70%; left: 80%; }
  33.33% { top: 30%; left: 10%; }
  50% { top: 60%; left: 60%; }
  66.66% { top: 20%; left: 70%; }
  83.33% { top: 80%; left: 30%; }
  100% { top: 10%; left: 20%; }
}
```

### 6. Spinning Orbit Animation

**Effect**: Image orbits around the center while spinning

```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  position: relative;
}

img {
  width: 100px;
  height: 100px;
  object-fit: contain;
  position: absolute;
  top: 50%;
  left: 50%;
  transform-origin: 0 0;
  animation: orbit 8s linear infinite, spin 2s linear infinite;
}

@keyframes orbit {
  from { transform: translate(-50%, -50%) rotate(0deg) translateX(200px); }
  to { transform: translate(-50%, -50%) rotate(360deg) translateX(200px); }
}

@keyframes spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}
```

## Important Rules & Tips

### 1. **Always Include Body Base Styles**
```css
body {
  margin: 0;
  padding: 0;
  background: transparent;
  width: 100vw;
  height: 100vh;
  overflow: hidden;
}
```

### 2. **Use Correct Calc() Values**
❌ **Wrong**: `calc(100% - 100%)`
✅ **Correct**: `calc(100vw - 200px)` (where 200px is your image width)

### 3. **Position Methods**

**Absolute Positioning** (for precise control):
```css
img {
  position: absolute;
  top: 0;
  left: 0;
}
```

**Flex Centering** (for center-based animations):
```css
body {
  display: flex;
  align-items: center;
  justify-content: center;
}
```

### 4. **Animation Timing Functions**

- `step-start` - Instant jumps
- `linear` - Constant speed
- `ease-in-out` - Smooth acceleration/deceleration
- `cubic-bezier()` - Custom curves

### 5. **Video-Specific Considerations**

For video content, use `video` instead of `img`:

```css
video {
  width: 400px;
  height: 300px;
  object-fit: cover;
  animation: myAnimation 5s infinite;
}
```

## CSS Visibility Modes

**Important**: VortiDeck offers **two CSS handling modes** to balance ad protection with user flexibility:

### LOCKED Mode (CSS Embedded in HTML)
- **CSS is hidden** from OBS browser source properties
- **Cannot be inspected or modified** through OBS interface
- **Perfect for ads** and protected content
- Used automatically for **free users** and when `css_locked: true`

### EDITABLE Mode (CSS Visible in OBS)
- **CSS appears in OBS** browser source custom CSS field
- **Can be inspected, learned from, and modified**
- **Great for premium users** and educational content
- Used for **premium users** by default and when `css_locked: false`

## API Parameters

### Basic Banner (Auto-determined CSS mode):
```json
{
  "actionId": "obs_banner_set_data",
  "url": "https://your-image-url.gif",
  "css": "/* your CSS here */",
  "width": 400,
  "height": 100
}
```

### Explicit CSS Control:
```json
{
  "actionId": "obs_banner_set_data",
  "url": "https://your-image-url.gif",
  "css": "/* your CSS here */",
  "width": 400,
  "height": 100,
  "css_locked": false
}
```

**Parameters:**
- `css_locked: false` - CSS visible in OBS (EDITABLE)
- `css_locked: true` - CSS embedded in HTML (LOCKED)
- **Omitted** - Auto-determined based on user type

### When to Use Each Mode

**Use LOCKED Mode** (`css_locked: true`):
- 🎯 **Ad campaigns** with specific animation requirements
- 🔒 **Brand-protected content** that shouldn't be modified
- 📦 **Packaged animations** for distribution
- 🛡️ **Content where CSS tampering breaks functionality**

**Use EDITABLE Mode** (`css_locked: false`):
- 🎓 **Learning and experimentation** 
- 🎨 **Custom user creations**
- 🔧 **Debugging animation issues**
- 💎 **Premium user flexibility**
- 📖 **Educational examples and tutorials**

## Testing Your Animations

1. **Send your banner command** using the API format above

2. **Check OBS**: Your banner should appear as a full-screen overlay with transparent background

3. **Inspect CSS** (if EDITABLE mode): Right-click banner source → Properties → Custom CSS

4. **Adjust timing**: Modify animation durations and delays as needed

## Common Issues & Solutions

### Issue: Animation Not Working
**Solution**: Check that you're targeting the right element (`img` vs `video`)

### Issue: Black Background
**Solution**: Ensure `background: transparent` is set on body

### Issue: Image Cut Off
**Solution**: Check your calc() values match your image dimensions

### Issue: Animation Too Fast/Slow
**Solution**: Adjust the duration in your animation property (e.g., `8s` → `12s`)

### Issue: Positioning Off
**Solution**: Use `box-sizing: border-box` and check margin/padding values

## Advanced Techniques

### Multiple Animations
```css
img {
  animation: 
    moveAround 10s linear infinite,
    pulse 3s ease-in-out infinite,
    rotate 5s linear infinite;
}
```

### Conditional Animations (hover-like effects)
```css
img {
  animation: idle 3s ease-in-out infinite;
}

/* Animation changes after 10 seconds */
img:nth-child(1) {
  animation-delay: 10s;
  animation-name: active;
}
```

### Responsive Animations
```css
img {
  width: 1vw; /* 10% of viewport width */
  height: 1vh; /* 10% of viewport height */
}
```

## Size Reference

Your banner browser source will always be **full OBS canvas size**:
- 1920x1080 (1080p)
- 2560x1440 (1440p) 
- 3840x2160 (4K)

Use `100vw` and `100vh` to reference the full canvas dimensions in your animations.

---

**Happy animating!** 🎬✨ Your banners will now smoothly animate across your OBS scenes with perfect transparency. 