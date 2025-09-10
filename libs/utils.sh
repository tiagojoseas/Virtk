#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_command() {
    if ! command -v "$1" &> /dev/null; then
        log_error "Command '$1' not found"
        return 1
    fi
    return 0
}

parse_yaml_subkeys() {
    local yaml_file="$1"
    local first_key="$2"
    local remaining_keys="$3"
    local indent_level="$4"
    
    # Build indentation pattern
    local indent_pattern=""
    for ((i=0; i<indent_level; i++)); do
        indent_pattern+="  "
    done
    
    # Find the section for the first key
    local in_section=false
    local temp_file
    temp_file=$(mktemp)
    
    while IFS= read -r line; do
        # Check current line indentation
        local line_indent=""
        if [[ "$line" =~ ^([[:space:]]*) ]]; then
            line_indent="${BASH_REMATCH[1]}"
        fi
        
        # Check if we found our key at the correct indentation level
        if [[ "$line" =~ ^${indent_pattern}${first_key}:[[:space:]]*$ ]] || [[ "$line" =~ ^${indent_pattern}${first_key}:[[:space:]]+.+ ]]; then
            in_section=true
            # If this line has a value, extract it (for simple key:value on same line)
            if [[ "$line" =~ ^${indent_pattern}${first_key}:[[:space:]]+(.+)$ ]] && [[ "$remaining_keys" == "" ]]; then
                local value="${BASH_REMATCH[1]}"
                value=$(echo "$value" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | tr -d '"'"'"'')
                echo "$value"
                rm -f "$temp_file"
                return 0
            fi
            continue
        fi
        
        # If we're in the section, collect lines with deeper indentation
        if [ "$in_section" = true ]; then
            # If we encounter a line at the same or lesser indentation, we've left the section
            if [[ ${#line_indent} -le ${#indent_pattern} ]] && [[ "$line" =~ ^[[:space:]]*[^[:space:]] ]]; then
                break
            fi
            
            # Add lines that belong to this section
            if [[ ${#line_indent} -gt ${#indent_pattern} ]] || [[ "$line" =~ ^[[:space:]]*$ ]]; then
                echo "$line" >> "$temp_file"
            fi
        fi
    done < "$yaml_file"
    
    # Recursively parse the remaining keys with increased indentation
    if [ -s "$temp_file" ]; then
        local result
        result=$(parse_yaml "$temp_file" "$remaining_keys" $((indent_level + 1)))
        local exit_code=$?
        rm -f "$temp_file"
        if [ $exit_code -eq 0 ]; then
            echo "$result"
            return 0
        fi
    fi
    
    rm -f "$temp_file"
    echo ""
    return 0
}

parse_yaml() {
    local yaml_file="$1"
    local key="$2"
    local indent_level="${3:-0}"  # Default indentation level is 0
    
    if [ ! -f "$yaml_file" ]; then
        log_error "YAML file not found: $yaml_file"
        echo ""
        return 0
    fi
    
    # Handle nested keys (e.g., kernel.version.number)
    if [[ "$key" == *.* ]]; then
        local first_key="${key%%.*}"
        local remaining_keys="${key#*.}"
        
        # Use yq if available and it's the top level call
        if [ "$indent_level" -eq 0 ] && command -v yq &> /dev/null; then
            local result
            result=$(yq eval ".$key" "$yaml_file" 2>/dev/null)
            if [ $? -eq 0 ] && [ "$result" != "null" ]; then
                echo "$result"
            else
                echo ""
            fi
            return 0
        fi
        
        # Use the dedicated subkey parsing function
        parse_yaml_subkeys "$yaml_file" "$first_key" "$remaining_keys" "$indent_level"
        return 0
    else
        # Simple key-value pairs at current indentation level
        local indent_pattern=""
        for ((i=0; i<indent_level; i++)); do
            indent_pattern+="  "
        done
        
        local result
        result=$(grep "^${indent_pattern}${key}:" "$yaml_file" | cut -d':' -f2- | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | tr -d '"'"'"'')
        
        if [ -n "$result" ]; then
            echo "$result"
        else
            echo ""
        fi
        return 0
    fi
}

get_yaml_subkeys() {
    local yaml_file="$1"
    local parent_key="$2"
    local indent_level="${3:-0}"
    
    if [ ! -f "$yaml_file" ]; then
        log_error "YAML file not found: $yaml_file"
        return 1
    fi
    
    # If parent_key contains dots, we need to navigate to the nested section first
    if [[ "$parent_key" == *.* ]]; then
        local first_key="${parent_key%%.*}"
        local remaining_keys="${parent_key#*.}"
        
        # Build indentation pattern for the first key
        local parent_indent=""
        for ((i=0; i<indent_level; i++)); do
            parent_indent+="  "
        done
        
        # Find the section for the first key and create a temp file with its content
        local in_section=false
        local temp_file
        temp_file=$(mktemp)
        
        while IFS= read -r line; do
            # Check current line indentation
            local line_indent=""
            if [[ "$line" =~ ^([[:space:]]*) ]]; then
                line_indent="${BASH_REMATCH[1]}"
            fi
            
            # Check if we found the first key at the correct indentation level
            if [[ "$line" =~ ^${parent_indent}${first_key}:[[:space:]]*$ ]] || [[ "$line" =~ ^${parent_indent}${first_key}:[[:space:]]+.+ ]]; then
                in_section=true
                continue
            fi
            
            # If we're in the section, collect lines with deeper indentation
            if [ "$in_section" = true ]; then
                # If we encounter a line at the same or lesser indentation, we've left the section
                if [[ ${#line_indent} -le ${#parent_indent} ]] && [[ "$line" =~ ^[[:space:]]*[^[:space:]] ]]; then
                    break
                fi
                
                # Add lines that belong to this section
                if [[ ${#line_indent} -gt ${#parent_indent} ]] || [[ "$line" =~ ^[[:space:]]*$ ]]; then
                    echo "$line" >> "$temp_file"
                fi
            fi
        done < "$yaml_file"
        
        # Recursively call get_yaml_subkeys with the remaining keys
        if [ -s "$temp_file" ]; then
            local result
            result=$(get_yaml_subkeys "$temp_file" "$remaining_keys" $((indent_level + 1)))
            local exit_code=$?
            rm -f "$temp_file"
            if [ $exit_code -eq 0 ]; then
                echo "$result"
                return 0
            fi
        fi
        
        rm -f "$temp_file"
        return 1
    else
        # Single level key - original logic
        # Build indentation pattern for the parent key
        local parent_indent=""
        for ((i=0; i<indent_level; i++)); do
            parent_indent+="  "
        done
        
        # Build indentation pattern for the child keys (one level deeper)
        local child_indent="${parent_indent}  "
        
        # If we have a parent key, find its section first
        if [ -n "$parent_key" ]; then
            local in_section=false
            local found_keys=()
            
            while IFS= read -r line; do
                # Check current line indentation
                local line_indent=""
                if [[ "$line" =~ ^([[:space:]]*) ]]; then
                    line_indent="${BASH_REMATCH[1]}"
                fi
                
                # Check if we found the parent key at the correct indentation level
                if [[ "$line" =~ ^${parent_indent}${parent_key}:[[:space:]]*$ ]] || [[ "$line" =~ ^${parent_indent}${parent_key}:[[:space:]]+.+ ]]; then
                    in_section=true
                    continue
                fi
                
                # If we're in the section, look for child keys
                if [ "$in_section" = true ]; then
                    # If we encounter a line at the same or lesser indentation, we've left the section
                    if [[ ${#line_indent} -le ${#parent_indent} ]] && [[ "$line" =~ ^[[:space:]]*[^[:space:]] ]]; then
                        break
                    fi
                    
                    # Check if this line contains a key at the immediate child level
                    if [[ "$line" =~ ^${child_indent}([^[:space:]]+):[[:space:]]*.*$ ]]; then
                        local key_name="${BASH_REMATCH[1]}"
                        found_keys+=("$key_name")
                    fi
                fi
            done < "$yaml_file"
            
            # Print found keys
            if [ ${#found_keys[@]} -gt 0 ]; then
                printf '%s\n' "${found_keys[@]}"
                return 0
            else
                return 1
            fi
        else
            # If no parent key specified, list top-level keys
            grep "^[^[:space:]].*:" "$yaml_file" | sed 's/:.*$//' | sort -u
        fi
    fi
}

get_config_array() {
    local yaml_file="$1"
    local section="$2"

    awk "/$section:/,/^[^ ]/ { if (/^ *- /) print \$2 }" "$yaml_file"
}
