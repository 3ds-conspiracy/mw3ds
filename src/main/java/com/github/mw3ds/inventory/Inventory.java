package com.github.mw3ds.inventory;

import com.github.mw3ds.item.Item;

import java.util.ArrayList;
import java.util.List;

/**
 * Player inventory system.
 */
public class Inventory {

    private List<Item> items = new ArrayList<>();
    private int maxSlots;

    public Inventory(int maxSlots) {
        this.maxSlots = maxSlots;
    }

    public boolean addItem(Item item) {
        if (items.size() >= maxSlots) {
            return false;
        }
        items.add(item);
        return true;
    }

    public boolean removeItem(Item item) {
        return items.remove(item);
    }

    public boolean removeItemByName(String itemName) {
        for (int i = 0; i < items.size(); i++) {
            if (items.get(i).getName().equals(itemName)) {
                items.remove(i);
                return true;
            }
        }
        return false;
    }

    public boolean hasItem(String itemName) {
        for (Item item : items) {
            if (item.getName().equals(itemName)) {
                return true;
            }
        }
        return false;
    }

    public List<Item> getItems() {
        return new ArrayList<>(items);
    }

    public int size() {
        return items.size();
    }

    public boolean isEmpty() {
        return items.isEmpty();
    }
}
