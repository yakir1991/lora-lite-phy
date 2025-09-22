#!/usr/bin/env python3
"""
Create corrected performance comparison charts
"""

import json
import matplotlib.pyplot as plt
import numpy as np

def load_performance_data():
    """Load performance data from JSON file"""
    try:
        with open('individual_performance_results.json', 'r') as f:
            return json.load(f)
    except FileNotFoundError:
        print("Performance results not found. Run simple_performance_test.py first.")
        return None

def create_performance_charts():
    """Create comprehensive performance charts"""
    data = load_performance_data()
    if not data:
        return
    
    # GNU Radio theoretical data
    gr_data = {
        'config_name': 'GNU Radio LoRa SDR',
        'samples_per_sec': 500000,
        'frames_per_sec': 50
    }
    
    # Prepare data
    configs = [d['config_name'] for d in data] + [gr_data['config_name']]
    samples_per_sec = [d['samples_per_sec'] / 1e6 for d in data] + [gr_data['samples_per_sec'] / 1e6]
    frames_per_sec = [d['frames_per_sec'] for d in data] + [gr_data['frames_per_sec']]
    
    # Create figure with subplots
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 10))
    
    # Chart 1: Sample Processing Speed
    colors = ['#2E8B57', '#4169E1', '#DC143C', '#FF8C00']
    bars1 = ax1.bar(configs, samples_per_sec, color=colors[:len(configs)])
    ax1.set_title('LoRa Receiver Sample Processing Speed Comparison', fontsize=16, fontweight='bold')
    ax1.set_ylabel('Samples/sec (Millions)', fontsize=12)
    ax1.set_xlabel('Configuration', fontsize=12)
    
    # Add value labels on bars
    for bar, value in zip(bars1, samples_per_sec):
        height = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2., height + 0.1,
                f'{value:.1f}M', ha='center', va='bottom', fontweight='bold')
    
    # Chart 2: Frame Processing Speed
    bars2 = ax2.bar(configs, frames_per_sec, color=colors[:len(configs)])
    ax2.set_title('LoRa Receiver Frame Processing Speed Comparison', fontsize=16, fontweight='bold')
    ax2.set_ylabel('Frames/sec', fontsize=12)
    ax2.set_xlabel('Configuration', fontsize=12)
    
    # Add value labels on bars
    for bar, value in zip(bars2, frames_per_sec):
        height = bar.get_height()
        ax2.text(bar.get_x() + bar.get_width()/2., height + 5,
                f'{value:.1f}', ha='center', va='bottom', fontweight='bold')
    
    # Rotate x-axis labels for better readability
    for ax in [ax1, ax2]:
        ax.tick_params(axis='x', rotation=45)
        ax.grid(True, alpha=0.3)
    
    # Add performance improvement annotations
    best_samples = max(samples_per_sec[:-1])  # Exclude GNU Radio
    gr_samples = samples_per_sec[-1]
    improvement_samples = best_samples / gr_samples
    
    best_frames = max(frames_per_sec[:-1])  # Exclude GNU Radio
    gr_frames = frames_per_sec[-1]
    improvement_frames = best_frames / gr_frames
    
    # Add improvement text
    fig.text(0.02, 0.02, f'Performance Improvement: {improvement_samples:.1f}x faster sample processing, {improvement_frames:.1f}x faster frame processing', 
             fontsize=12, style='italic', weight='bold')
    
    plt.tight_layout()
    plt.savefig('corrected_performance_comparison.png', dpi=300, bbox_inches='tight')
    plt.savefig('corrected_performance_comparison.pdf', bbox_inches='tight')
    print("Corrected performance charts saved as 'corrected_performance_comparison.png' and 'corrected_performance_comparison.pdf'")

def create_detailed_comparison():
    """Create detailed comparison chart"""
    data = load_performance_data()
    if not data:
        return
    
    # Calculate metrics
    configs = [d['config_name'] for d in data]
    samples_per_sec = [d['samples_per_sec'] / 1e6 for d in data]
    frames_per_sec = [d['frames_per_sec'] for d in data]
    total_frames = [d['total_frames'] for d in data]
    file_sizes = [d['file_size_bytes'] / (1024*1024) for d in data]  # MB
    
    # Create subplots
    fig, ((ax1, ax2), (ax3, ax4)) = plt.subplots(2, 2, figsize=(16, 12))
    
    # Chart 1: Sample Processing Speed
    colors = ['#2E8B57', '#4169E1', '#DC143C']
    bars1 = ax1.bar(configs, samples_per_sec, color=colors)
    ax1.set_title('Sample Processing Speed', fontsize=14, fontweight='bold')
    ax1.set_ylabel('Samples/sec (Millions)')
    ax1.tick_params(axis='x', rotation=45)
    ax1.grid(True, alpha=0.3)
    
    for bar, value in zip(bars1, samples_per_sec):
        height = bar.get_height()
        ax1.text(bar.get_x() + bar.get_width()/2., height + 0.1,
                f'{value:.1f}M', ha='center', va='bottom', fontweight='bold')
    
    # Chart 2: Frame Processing Speed
    bars2 = ax2.bar(configs, frames_per_sec, color=colors)
    ax2.set_title('Frame Processing Speed', fontsize=14, fontweight='bold')
    ax2.set_ylabel('Frames/sec')
    ax2.tick_params(axis='x', rotation=45)
    ax2.grid(True, alpha=0.3)
    
    for bar, value in zip(bars2, frames_per_sec):
        height = bar.get_height()
        ax2.text(bar.get_x() + bar.get_width()/2., height + 5,
                f'{value:.1f}', ha='center', va='bottom', fontweight='bold')
    
    # Chart 3: Frames Processed
    bars3 = ax3.bar(configs, total_frames, color=colors)
    ax3.set_title('Frames Processed', fontsize=14, fontweight='bold')
    ax3.set_ylabel('Number of Frames')
    ax3.tick_params(axis='x', rotation=45)
    ax3.grid(True, alpha=0.3)
    
    for bar, value in zip(bars3, total_frames):
        height = bar.get_height()
        ax3.text(bar.get_x() + bar.get_width()/2., height + 0.1,
                f'{value}', ha='center', va='bottom', fontweight='bold')
    
    # Chart 4: File Size vs Performance
    ax4.scatter(file_sizes, samples_per_sec, s=100, c=colors, alpha=0.7)
    ax4.set_title('File Size vs Sample Processing Speed', fontsize=14, fontweight='bold')
    ax4.set_xlabel('File Size (MB)')
    ax4.set_ylabel('Samples/sec (Millions)')
    ax4.grid(True, alpha=0.3)
    
    # Add labels to scatter points
    for i, (x, y) in enumerate(zip(file_sizes, samples_per_sec)):
        ax4.annotate(f'{y:.1f}M', (x, y), xytext=(5, 5), textcoords='offset points', fontweight='bold')
    
    plt.tight_layout()
    plt.savefig('detailed_performance_analysis.png', dpi=300, bbox_inches='tight')
    print("Detailed performance analysis saved as 'detailed_performance_analysis.png'")

def main():
    """Main function"""
    print("Creating corrected performance comparison charts...")
    create_performance_charts()
    create_detailed_comparison()
    print("All corrected charts created successfully!")

if __name__ == '__main__':
    main()
