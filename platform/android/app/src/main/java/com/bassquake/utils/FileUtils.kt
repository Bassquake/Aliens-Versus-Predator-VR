package com.bassquake.avpvr.utils

import android.content.Context
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

fun copyAllAssetsToInternalStorage(context: Context, path: String = "") {
    val assetManager = context.assets

    // Get all contents of the current folder
    val assets = try {
        assetManager.list(path) ?: emptyArray()
    } catch (e: IOException) {
        emptyArray()
    }

    for (assetName in assets) {
        // Filter system-injected folders
        if (path.isEmpty() && (assetName == "images" || assetName == "webkit")) continue

        val assetPath = if (path.isEmpty()) assetName else "$path/$assetName"

        // Determine if it is a directory by attempting to list it
        val subContents = try {
            assetManager.list(assetPath)
        } catch (e: IOException) {
            null
        }

        if (!subContents.isNullOrEmpty()) {
            // It is a directory: Create it and recurse
            val dir = File(context.filesDir, assetPath)
            if (!dir.exists()) dir.mkdirs()
            copyAllAssetsToInternalStorage(context, assetPath)
        } else {
            // It is a file: Copy it
            val outputFile = File(context.filesDir, assetPath)
            if (!outputFile.exists()) {
                copySingleAsset(context, assetPath, outputFile)
            }
        }
    }
}

private fun copySingleAsset(context: Context, assetPath: String, outputFile: File) {
    try {
        context.assets.open(assetPath).use { input ->
            FileOutputStream(outputFile).use { output ->
                input.copyTo(output)
            }
        }
    } catch (e: Exception) {
        Log.e("FileUtils", "Failed to copy $assetPath", e)
    }
}