# Maintenance

## Requirements

- Docker
- Git

## Usage

```bash
# Sync with main branch
./maintain.sh sync

# Update libraries
./maintain.sh update all      # Update all libraries
./maintain.sh update miniz    # Update only miniz
./maintain.sh update luajit   # Update only LuaJIT
```

## Notes

- The Dockerfile uses Ubuntu 20.04 (GLIBC 2.31) for compatibility
- You can modify `Dockerfile.maintenance` to use a different base image if needed
- After updating, test locally before committing any changes
