#!/usr/bin/env python3
"""
Test script for VortiDeck OBS Banner WebSocket API

This script demonstrates how to control the banner functionality via WebSocket commands.
The plugin runs a WebSocket server on ws://localhost:9001

Available commands:
- show_banner: Shows the banner in the current scene
- hide_banner: Hides the banner from all scenes  
- set_banner: Sets the banner content (requires file_path parameter)

Usage:
    python test_banner_api.py
"""

import asyncio
import websockets
import json
import sys
import os

WEBSOCKET_URL = "ws://localhost:9001"

async def send_command(websocket, command, **kwargs):
    """Send a command to the banner WebSocket server"""
    message = {
        "command": command,
        **kwargs
    }
    
    print(f"Sending: {json.dumps(message, indent=2)}")
    await websocket.send(json.dumps(message))
    print("Command sent successfully!")

async def test_banner_api():
    """Test the banner API functionality"""
    try:
        async with websockets.connect(WEBSOCKET_URL) as websocket:
            print(f"Connected to {WEBSOCKET_URL}")
            print("=" * 50)
            
            while True:
                print("\nAvailable commands:")
                print("1. show_banner - Show the banner")
                print("2. hide_banner - Hide the banner")
                print("3. set_banner - Set banner content (requires file path)")
                print("4. exit - Exit the test")
                
                choice = input("\nEnter your choice (1-4): ").strip()
                
                if choice == "1":
                    await send_command(websocket, "show_banner")
                
                elif choice == "2":
                    await send_command(websocket, "hide_banner")
                
                elif choice == "3":
                    file_path = input("Enter the full path to image/video file: ").strip()
                    if not os.path.exists(file_path):
                        print(f"Error: File does not exist: {file_path}")
                        continue
                    await send_command(websocket, "set_banner", file_path=file_path)
                
                elif choice == "4":
                    print("Exiting...")
                    break
                
                else:
                    print("Invalid choice. Please enter 1-4.")
                
                print("-" * 30)
    
    except websockets.exceptions.ConnectionRefused:
        print(f"Error: Could not connect to {WEBSOCKET_URL}")
        print("Make sure the OBS plugin is loaded and the WebSocket server is running.")
        return False
    
    except KeyboardInterrupt:
        print("\nTest interrupted by user.")
        return False
    
    except Exception as e:
        print(f"Error: {e}")
        return False
    
    return True

def main():
    """Main function"""
    print("VortiDeck OBS Banner WebSocket API Test")
    print("=" * 50)
    print(f"Connecting to {WEBSOCKET_URL}...")
    
    # Run the async test
    asyncio.run(test_banner_api())

if __name__ == "__main__":
    main()