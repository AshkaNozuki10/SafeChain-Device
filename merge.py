import os
import glob

# The folders you want to extract code from
folders = ["Safechain_Repeater", "Safechain_Node", "Safechain_Gateway"]

print("Starting code extraction...\n")

for folder in folders:
    # Check if the folder exists to prevent errors
    if not os.path.exists(folder):
        print(f"❌ Skipping {folder}: Folder not found.")
        continue

    # Find all relevant code files inside the specific folder
    search_pattern_ino = os.path.join(folder, "*.ino")
    search_pattern_h = os.path.join(folder, "*.h")
    search_pattern_cpp = os.path.join(folder, "*.cpp")
    
    # Combine the search results
    files = glob.glob(search_pattern_ino) + glob.glob(search_pattern_h) + glob.glob(search_pattern_cpp)

    # If the folder actually has code files, combine them
    if files:
        # Create a text file named after the folder (e.g., Safechain_Node.txt)
        output_filename = f"{folder}.txt"
        
        # Open in "w" mode so it replaces the file entirely every time you run it
        with open(output_filename, "w", encoding="utf-8") as outfile:
            for file in files:
                # Extract just the file name (e.g., "config.h") for a clean header
                just_filename = os.path.basename(file)
                
                outfile.write(f"\n{'='*50}\n")
                outfile.write(f"FILE: {just_filename}\n")
                outfile.write(f"{'='*50}\n\n") # <-- FIXED LINE
                
                # Read the actual code and append it
                with open(file, "r", encoding="utf-8") as infile:
                    outfile.write(infile.read())
                    outfile.write("\n\n") # Add some breathing room between files
                    
        print(f"✅ Successfully updated {output_filename} ({len(files)} files merged)")
    else:
        print(f"⚠️ No .ino, .h, or .cpp files found in {folder}.")

print("\nAll done!")