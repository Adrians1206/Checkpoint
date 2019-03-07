/*
*   This file is part of Checkpoint
*   Copyright (C) 2017-2019 Bernardo Giordano, FlagBrew
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

#include "io.hpp"

bool io::fileExists(const std::string& path)
{
    struct stat buffer;   
    return (stat (path.c_str(), &buffer) == 0);
}

void io::copyFile(const std::string& srcPath, const std::string& dstPath)
{
    std::fstream src(srcPath, std::ios::in | std::ios::binary);
    std::fstream dst(dstPath, std::ios::out | std::ios::binary);
    if (!src.is_open() || !dst.is_open())
    {
        return;
    }
     
    src.seekg(0, src.end);
    u64 sz = src.tellg();
    src.seekg(0, src.beg);

    u8* buf = new u8[BUFFER_SIZE];
    u64 offset = 0;
    size_t slashpos = srcPath.rfind("/");
    std::string name = srcPath.substr(slashpos + 1, srcPath.length() - slashpos - 1);
    while (offset < sz) {
        src.read((char*)buf, BUFFER_SIZE);
        dst.write((char*)buf, src.gcount());
        offset += src.gcount();
        Gui::drawCopy(name, offset, sz);
    }

    delete[] buf;
    src.close();
    dst.close();

    // commit each file to the save
    if (dstPath.rfind("save:/", 0) == 0)
    {
        fsdevCommitDevice("save");
    }
}

Result io::copyDirectory(const std::string& srcPath, const std::string& dstPath)
{
    Result res = 0;
    bool quit = false;
    Directory items(srcPath);
    
    if (!items.good())
    {
        return items.error();
    }
    
    for (size_t i = 0, sz = items.size(); i < sz && !quit; i++)
    {
        std::string newsrc = srcPath + items.entry(i);
        std::string newdst = dstPath + items.entry(i);
        
        if (items.folder(i))
        {
            res = io::createDirectory(newdst);
            if (R_SUCCEEDED(res))
            {
                newsrc += "/";
                newdst += "/";
                res = io::copyDirectory(newsrc, newdst);
            }
            else
            {
                quit = true;
            }
        }
        else
        {
            io::copyFile(newsrc, newdst);
        }
    }
    
    return 0;
}

Result io::createDirectory(const std::string& path)
{
    mkdir(path.c_str(), 777);
    return 0;
}

bool io::directoryExists(const std::string& path)
{
    struct stat sb;
    return (stat(path.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode));
}

// https://stackoverflow.com/questions/2256945/removing-a-non-empty-directory-programmatically-in-c-or-c
Result io::deleteFolderRecursively(const std::string& path)
{
    Directory dir(path);
    if (!dir.good())
    {
        return dir.error();
    }

    for (size_t i = 0, sz = dir.size(); i < sz; i++)
    {
        if (dir.folder(i))
        {
            std::string newpath = path + "/" + dir.entry(i) + "/";
            deleteFolderRecursively(newpath);
            newpath = path + dir.entry(i);
            rmdir(newpath.c_str());
        }
        else
        {
            std::string newpath = path + dir.entry(i);
            std::remove(newpath.c_str());
        }
    }

    rmdir(path.c_str());
    return 0;
}

void io::backup(size_t index, u128 uid)
{
    // check if multiple selection is enabled and don't ask for confirmation if that's the case
    if (!Gui::multipleSelectionEnabled())
    {
        if (!Gui::askForConfirmation("Backup selected save?"))
        {
            return;
        }
    }

    const size_t cellIndex = Gui::index(CELLS);
    const bool isNewFolder = cellIndex == 0;
    Result res = 0;
    
    Title title;
    getTitle(title, uid, index);
    
    FsFileSystem fileSystem;
    res = FileSystem::mount(&fileSystem, title.id(), title.userId());
    if (R_SUCCEEDED(res))
    {
        int ret = FileSystem::mount(fileSystem);
        if (ret == -1)
        {
            FileSystem::unmount();
            Gui::createError(-2, "Failed to mount save.");
            return;
        }
    }
    else
    {
        Gui::createError(res, "Failed to mount save.");
        return;
    }

    std::string suggestion = DateTime::dateTimeStr() + " " + (StringUtils::containsInvalidChar(Account::username(title.userId())) ? "" : StringUtils::removeNotAscii(StringUtils::removeAccents(Account::username(title.userId()))));
    std::string customPath;

    if (Gui::multipleSelectionEnabled())
    {
        customPath = isNewFolder ? suggestion : "";
    }
    else
    {
        if (isNewFolder)
        {
            std::pair<bool, std::string> keyboardResponse = KeyboardManager::get().keyboard(suggestion);
            if (keyboardResponse.first)
            {
                customPath = StringUtils::removeForbiddenCharacters(keyboardResponse.second);
            }
            else
            {
                FileSystem::unmount();
                return;
            }
        }
        else
        {
            customPath = "";
        }
    }
    
    std::string dstPath;
    if (!isNewFolder)
    {
        // we're overriding an existing folder
        dstPath = title.fullPath(cellIndex);
    }
    else
    {
        dstPath = title.path() + "/" + customPath;
    } 

    if (!isNewFolder || io::directoryExists(dstPath))
    {
        int ret = io::deleteFolderRecursively((dstPath + "/").c_str());
        if (ret != 0)
        {
            FileSystem::unmount();
            Gui::createError((Result)ret, "Failed to delete the existing backup\ndirectory recursively.");
            return;
        }
    }
    
    res = io::createDirectory(dstPath);
    res = io::copyDirectory("save:/", dstPath + "/");
    if (R_FAILED(res))
    {
        FileSystem::unmount();
        io::deleteFolderRecursively((dstPath + "/").c_str());
        Gui::createError(res, "Failed to backup save.");
        return;
    }
    
    refreshDirectories(title.id());
    
    FileSystem::unmount();
    Gui::createInfo("Success!", "Progress correctly saved to disk.");
}

void io::restore(size_t index, u128 uid)
{
    const size_t cellIndex = Gui::index(CELLS);
    if (cellIndex == 0 || !Gui::askForConfirmation("Restore selected save?"))
    {
        return;
    }
    
    Result res = 0;
    
    Title title;
    getTitle(title, uid, index);
    
    FsFileSystem fileSystem;
    res = title.systemSave() ? FileSystem::mount(&fileSystem, title.id()) : FileSystem::mount(&fileSystem, title.id(), title.userId());
    if (R_SUCCEEDED(res))
    {
        int ret = FileSystem::mount(fileSystem);
        if (ret == -1)
        {
            FileSystem::unmount();
            Gui::createError(-2, "Failed to mount save.");
            return;
        }
    }
    else
    {
        Gui::createError(res, "Failed to mount save.");
        return;        
    }

    std::string srcPath = title.fullPath(cellIndex) + "/";
    std::string dstPath = "save:/";
    
    res = io::deleteFolderRecursively(dstPath.c_str());
    if (R_FAILED(res))
    {
        FileSystem::unmount();
        Gui::createError(res, "Failed to delete save.");
        return;
    }
    
    res = io::copyDirectory(srcPath, dstPath);
    if (R_FAILED(res))
    {
        FileSystem::unmount();
        Gui::createError(res, "Failed to restore save.");
        return;
    }
    
    res = fsdevCommitDevice("save");
    if (R_FAILED(res))
    {
        Gui::createError(res, "Failed to commit to save device.");
    }
    else
    {
        Gui::createInfo("Success!", Gui::nameFromCell(cellIndex) + "\nhas been restored successfully.");
    }
    
    FileSystem::unmount();
}